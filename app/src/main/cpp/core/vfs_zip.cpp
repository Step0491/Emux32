#include "vfs_zip.h"

#include <android/log.h>
#include <zlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <algorithm>

#define TAG "EmuxZipVfs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

namespace {

#pragma pack(push, 1)

struct ZipEOCD {
    uint32_t signature;           // 0x06054b50
    uint16_t diskNumber;
    uint16_t cdStartDisk;
    uint16_t recordsOnDisk;
    uint16_t totalRecords;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLength;
};

struct ZipCDFH {
    uint32_t signature;           // 0x02014b50
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t compressionMethod;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength;
    uint16_t fileCommentLength;
    uint16_t diskNumberStart;
    uint16_t internalAttrs;
    uint32_t externalAttrs;
    uint32_t localHeaderOffset;
};

struct ZipLFH {
    uint32_t signature;           // 0x04034b50
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t compressionMethod;
    uint16_t lastModTime;
    uint16_t lastModDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength;
};

#pragma pack(pop)

constexpr uint32_t kSigEOCD = 0x06054b50U;
constexpr uint32_t kSigCDFH = 0x02014b50U;
constexpr uint32_t kSigLFH  = 0x04034b50U;

} // namespace

// ============================================================================
// VfsFileStream Implementation
// ============================================================================

VfsFileStream::VfsFileStream(const ZipEntry& entry, FILE* containerFp)
    : entry_(entry), containerFp_(containerFp), ownsFp_(true), currentPos_(0), isDecompressed_(false) {
    if (!containerFp_) {
        containerFp_ = fopen(entry_.archivePath.c_str(), "rb");
    }
}

VfsFileStream::~VfsFileStream() {
    if (ownsFp_ && containerFp_) {
        fclose(containerFp_);
        containerFp_ = nullptr;
    }
}

int VfsFileStream::getContainerFd() const {
    if (containerFp_) {
        return fileno(containerFp_);
    }
    return -1;
}

bool VfsFileStream::decompressFull() {
    if (isDecompressed_) return true;
    if (!containerFp_) return false;

    if (entry_.compressionMethod == ZIP_METHOD_STORED) {
        cachedDecompressedData_.resize(entry_.uncompressedSize);
        if (fseeko(containerFp_, entry_.dataPayloadOffset, SEEK_SET) != 0) {
            return false;
        }
        size_t rd = fread(cachedDecompressedData_.data(), 1, entry_.uncompressedSize, containerFp_);
        if (rd != entry_.uncompressedSize) {
            LOGW("VfsFileStream: Lettura parziale file STORED: %zu / %u", rd, entry_.uncompressedSize);
        }
        isDecompressed_ = true;
        return true;
    }

    if (entry_.compressionMethod == ZIP_METHOD_DEFLATED) {
        std::vector<uint8_t> compressedData(entry_.compressedSize);
        if (fseeko(containerFp_, entry_.dataPayloadOffset, SEEK_SET) != 0) {
            return false;
        }
        size_t rd = fread(compressedData.data(), 1, entry_.compressedSize, containerFp_);
        if (rd != entry_.compressedSize) {
            LOGE("VfsFileStream: Lettura compressa fallita per '%s'", entry_.internalPath.c_str());
            return false;
        }

        cachedDecompressedData_.resize(entry_.uncompressedSize);

        z_stream strm{};
        strm.next_in = compressedData.data();
        strm.avail_in = static_cast<uInt>(compressedData.size());
        strm.next_out = cachedDecompressedData_.data();
        strm.avail_out = static_cast<uInt>(cachedDecompressedData_.size());

        // -MAX_WBITS per raw DEFLATE stream (senza zlib/gzip wrapper standard ZIP)
        int initRes = inflateInit2(&strm, -MAX_WBITS);
        if (initRes != Z_OK) {
            LOGE("VfsFileStream: inflateInit2 fallito (%d) per '%s'", initRes, entry_.internalPath.c_str());
            return false;
        }

        int inflRes = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);

        if (inflRes != Z_STREAM_END && inflRes != Z_OK) {
            LOGE("VfsFileStream: inflate fallito (%d) per '%s'", inflRes, entry_.internalPath.c_str());
            return false;
        }

        isDecompressed_ = true;
        return true;
    }

    LOGE("VfsFileStream: Metodo di compressione non supportato (%u) per '%s'",
         entry_.compressionMethod, entry_.internalPath.c_str());
    return false;
}

const uint8_t* VfsFileStream::getBuffer() {
    if (!isDecompressed_) {
        if (!decompressFull()) {
            return nullptr;
        }
    }
    return cachedDecompressedData_.data();
}

size_t VfsFileStream::read(void* dest, size_t bytes) {
    if (!dest || bytes == 0 || currentPos_ >= static_cast<off_t>(entry_.uncompressedSize)) {
        return 0;
    }

    size_t available = static_cast<size_t>(entry_.uncompressedSize - currentPos_);
    size_t toRead = std::min(bytes, available);

    // Modalità STORED: lettura diretta dal file container con seek offset $O(1)$
    if (entry_.compressionMethod == ZIP_METHOD_STORED && !isDecompressed_) {
        if (!containerFp_) return 0;
        off_t targetOffset = entry_.dataPayloadOffset + currentPos_;
        if (fseeko(containerFp_, targetOffset, SEEK_SET) != 0) {
            return 0;
        }
        size_t actual = fread(dest, 1, toRead, containerFp_);
        currentPos_ += actual;
        return actual;
    }

    // Modalità DEFLATED o buffer in cache
    if (!isDecompressed_) {
        if (!decompressFull()) {
            return 0;
        }
    }

    std::memcpy(dest, cachedDecompressedData_.data() + currentPos_, toRead);
    currentPos_ += toRead;
    return toRead;
}

int VfsFileStream::seek(off_t offset, int whence) {
    off_t target = 0;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = currentPos_ + offset; break;
        case SEEK_END: target = static_cast<off_t>(entry_.uncompressedSize) + offset; break;
        default: return -1;
    }

    if (target < 0 || target > static_cast<off_t>(entry_.uncompressedSize)) {
        return -1;
    }

    currentPos_ = target;
    return 0;
}

off_t VfsFileStream::tell() const {
    return currentPos_;
}

// ============================================================================
// ZipArchiveVfs Implementation
// ============================================================================

ZipArchiveVfs& ZipArchiveVfs::getInstance() {
    static ZipArchiveVfs instance;
    return instance;
}

void ZipArchiveVfs::reset() {
    std::lock_guard<std::mutex> lock(vfsMutex_);
    entries_.clear();
    mountedArchives_.clear();
    LOGI("ZipArchiveVfs: Stato azzerato con successo");
}

std::string ZipArchiveVfs::normalizePath(const std::string& path) {
    std::string out;
    out.reserve(path.size());

    // Rimuove eventuali slash o backslash iniziali
    size_t start = 0;
    while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
        start++;
    }

    for (size_t i = start; i < path.size(); ++i) {
        char c = path[i];
        if (c == '\\') c = '/';
        out.push_back(c);
    }

    return out;
}

bool ZipArchiveVfs::parseCentralDirectory(const std::string& archivePath, FILE* fp, const std::string& prefix) {
    if (!fp) return false;

    if (fseeko(fp, 0, SEEK_END) != 0) return false;
    off_t fileSize = ftello(fp);
    if (fileSize < static_cast<off_t>(sizeof(ZipEOCD))) {
        LOGE("ZipArchiveVfs: File '%s' troppo piccolo per essere uno ZIP valido", archivePath.c_str());
        return false;
    }

    // Scansione degli ultimi 64 KB + sizeof(ZipEOCD) per individuare il marcatore EOCD
    size_t searchLen = std::min<size_t>(fileSize, 65536 + sizeof(ZipEOCD));
    off_t searchStart = fileSize - searchLen;
    if (fseeko(fp, searchStart, SEEK_SET) != 0) return false;

    std::vector<uint8_t> searchBuf(searchLen);
    if (fread(searchBuf.data(), 1, searchLen, fp) != searchLen) {
        return false;
    }

    off_t eocdOffset = -1;
    for (ssize_t i = searchLen - sizeof(ZipEOCD); i >= 0; --i) {
        uint32_t sig = *reinterpret_cast<const uint32_t*>(searchBuf.data() + i);
        if (sig == kSigEOCD) {
            eocdOffset = searchStart + i;
            break;
        }
    }

    if (eocdOffset < 0) {
        LOGW("ZipArchiveVfs: Marcatore EOCD non trovato in '%s'", archivePath.c_str());
        return false;
    }

    if (fseeko(fp, eocdOffset, SEEK_SET) != 0) return false;
    ZipEOCD eocd{};
    if (fread(&eocd, sizeof(ZipEOCD), 1, fp) != 1) return false;

    LOGI("ZipArchiveVfs: Indicizzazione '%s' (Record CD: %u, Offset: 0x%x, Dimensione: %u byte)...",
         archivePath.c_str(), eocd.totalRecords, eocd.cdOffset, eocd.cdSize);

    if (fseeko(fp, eocd.cdOffset, SEEK_SET) != 0) return false;

    size_t indexedCount = 0;
    for (uint16_t r = 0; r < eocd.totalRecords; ++r) {
        ZipCDFH cdfh{};
        if (fread(&cdfh, sizeof(ZipCDFH), 1, fp) != 1) break;
        if (cdfh.signature != kSigCDFH) {
            LOGW("ZipArchiveVfs: CDFH non valida all'indice %u (sig=0x%08x)", r, cdfh.signature);
            break;
        }

        std::string rawName(cdfh.fileNameLength, '\0');
        if (fread(rawName.data(), 1, cdfh.fileNameLength, fp) != cdfh.fileNameLength) break;

        // Salto i campi extra e i commenti
        if (cdfh.extraFieldLength > 0) {
            fseeko(fp, cdfh.extraFieldLength, SEEK_CUR);
        }
        if (cdfh.fileCommentLength > 0) {
            fseeko(fp, cdfh.fileCommentLength, SEEK_CUR);
        }

        // Salta le directory (terminano con '/')
        if (!rawName.empty() && rawName.back() == '/') {
            continue;
        }

        off_t curCdPos = ftello(fp);

        // Lettura del Local File Header per determinare l'esatto dataPayloadOffset
        if (fseeko(fp, cdfh.localHeaderOffset, SEEK_SET) == 0) {
            ZipLFH lfh{};
            if (fread(&lfh, sizeof(ZipLFH), 1, fp) == 1 && lfh.signature == kSigLFH) {
                off_t payloadOffset = cdfh.localHeaderOffset + sizeof(ZipLFH) + lfh.fileNameLength + lfh.extraFieldLength;

                ZipEntry entry;
                entry.archivePath = archivePath;
                entry.internalPath = normalizePath(rawName);
                entry.compressionMethod = cdfh.compressionMethod;
                entry.compressedSize = cdfh.compressedSize;
                entry.uncompressedSize = cdfh.uncompressedSize;
                entry.crc32 = cdfh.crc32;
                entry.localHeaderOffset = cdfh.localHeaderOffset;
                entry.dataPayloadOffset = payloadOffset;

                std::string norm = entry.internalPath;
                entries_[norm] = entry;

                // Indicizzazione con prefisso personalizzato se specificato
                if (!prefix.empty()) {
                    entries_[prefix + "/" + norm] = entry;
                }

                // Alias aggiuntivi: se inizia con "assets/", indicizza anche senza "assets/" e viceversa
                if (norm.rfind("assets/", 0) == 0) {
                    entries_[norm.substr(7)] = entry;
                } else {
                    entries_["assets/" + norm] = entry;
                }

                indexedCount++;
            }
        }

        // Ripristina la posizione al prossimo header della Central Directory
        fseeko(fp, curCdPos, SEEK_SET);
    }

    LOGI("ZipArchiveVfs: Indicizzati con successo %zu file da '%s'", indexedCount, archivePath.c_str());
    return indexedCount > 0;
}

bool ZipArchiveVfs::mountArchive(const std::string& archivePath, const std::string& prefix) {
    if (archivePath.empty()) return false;
    std::lock_guard<std::mutex> lock(vfsMutex_);

    // Evita montaggi duplicati dello stesso container
    for (const auto& m : mountedArchives_) {
        if (m == archivePath) return true;
    }

    FILE* fp = fopen(archivePath.c_str(), "rb");
    if (!fp) {
        LOGW("ZipArchiveVfs::mountArchive: Impossibile aprire file container '%s'", archivePath.c_str());
        return false;
    }

    bool success = parseCentralDirectory(archivePath, fp, prefix);
    fclose(fp);

    if (success) {
        mountedArchives_.push_back(archivePath);
        LOGI("ZipArchiveVfs: Montato container '%s' (Totale file VFS: %zu)",
             archivePath.c_str(), entries_.size());
    }

    return success;
}

size_t ZipArchiveVfs::mountObbDirectory(const std::string& obbDir) {
    if (obbDir.empty()) return 0;

    DIR* d = opendir(obbDir.c_str());
    if (!d) {
        LOGW("ZipArchiveVfs::mountObbDirectory: Impossibile aprire cartella '%s'", obbDir.c_str());
        return 0;
    }

    size_t mountedCount = 0;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string fname(ent->d_name);
        std::string fullPath = obbDir + "/" + fname;

        struct stat st{};
        if (stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            // Ricorsione controllata in sottocartelle (es: obb/com.turner.cardwars o data/com.glu.gladiator)
            mountedCount += mountObbDirectory(fullPath);
            continue;
        }

        std::string lower = fname;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        bool isArchive = false;
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".obb") isArchive = true;
        else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".zip") isArchive = true;
        else if (lower.size() >= 6 && lower.substr(lower.size() - 6) == ".cache") isArchive = true;

        if (isArchive) {
            if (mountArchive(fullPath)) {
                mountedCount++;
            }
        }
    }
    closedir(d);

    if (mountedCount > 0) {
        LOGI("ZipArchiveVfs: Rilevati e montati %zu archivi (.obb/.zip) da '%s'", mountedCount, obbDir.c_str());
    }
    return mountedCount;
}

bool ZipArchiveVfs::hasFile(const std::string& relativePath) const {
    if (relativePath.empty()) return false;
    std::string norm = normalizePath(relativePath);
    std::lock_guard<std::mutex> lock(vfsMutex_);
    return entries_.find(norm) != entries_.end();
}

bool ZipArchiveVfs::findEntry(const std::string& relativePath, ZipEntry& outEntry) const {
    if (relativePath.empty()) return false;
    std::string norm = normalizePath(relativePath);
    std::lock_guard<std::mutex> lock(vfsMutex_);
    auto it = entries_.find(norm);
    if (it != entries_.end()) {
        outEntry = it->second;
        return true;
    }

    // Prova rimozione prefisso "assets/" o aggiunta
    if (norm.rfind("assets/", 0) == 0) {
        auto itSub = entries_.find(norm.substr(7));
        if (itSub != entries_.end()) {
            outEntry = itSub->second;
            return true;
        }
    } else {
        auto itAdd = entries_.find("assets/" + norm);
        if (itAdd != entries_.end()) {
            outEntry = itAdd->second;
            return true;
        }
    }

    return false;
}

std::unique_ptr<VfsFileStream> ZipArchiveVfs::openStream(const std::string& relativePath) {
    ZipEntry entry;
    if (!findEntry(relativePath, entry)) {
        return nullptr;
    }

    FILE* fp = fopen(entry.archivePath.c_str(), "rb");
    if (!fp) {
        LOGE("ZipArchiveVfs::openStream: Container '%s' non accessibile per '%s'",
             entry.archivePath.c_str(), relativePath.c_str());
        return nullptr;
    }

    return std::make_unique<VfsFileStream>(entry, fp);
}

std::vector<std::string> ZipArchiveVfs::listDirectory(const std::string& dirPath) const {
    std::string norm = normalizePath(dirPath);
    if (!norm.empty() && norm.back() != '/') {
        norm.push_back('/');
    }

    std::vector<std::string> results;
    std::lock_guard<std::mutex> lock(vfsMutex_);

    for (const auto& pair : entries_) {
        const std::string& path = pair.first;
        if (path.rfind(norm, 0) == 0 && path.size() > norm.size()) {
            std::string sub = path.substr(norm.size());
            size_t slash = sub.find('/');
            std::string item = (slash != std::string::npos) ? sub.substr(0, slash) : sub;
            if (std::find(results.begin(), results.end(), item) == results.end()) {
                results.push_back(item);
            }
        }
    }

    return results;
}

size_t ZipArchiveVfs::getTotalIndexedFiles() const {
    std::lock_guard<std::mutex> lock(vfsMutex_);
    return entries_.size();
}

} // namespace emux
