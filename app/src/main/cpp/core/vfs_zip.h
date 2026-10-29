#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstdio>
#include <sys/types.h>

namespace emux {

/**
 * Metodi di compressione standard ZIP
 */
constexpr uint16_t ZIP_METHOD_STORED    = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED  = 8;

/**
 * Rappresenta un singolo file indicizzato all'interno di un archivio OBB o APK.
 */
struct ZipEntry {
    std::string archivePath;        // Percorso dell'archivio container (.obb o .apk)
    std::string internalPath;       // Percorso relativo interno normalizzato (es: "assets/bin/Data/mainData")
    uint16_t compressionMethod{0};  // 0 = STORED, 8 = DEFLATED
    uint32_t compressedSize{0};     // Dimensione su disco
    uint32_t uncompressedSize{0};   // Dimensione decompressa reale
    uint32_t crc32{0};              // Checksum CRC32
    off_t localHeaderOffset{0};     // Offset del Local File Header
    off_t dataPayloadOffset{0};     // Offset calcolato dei byte grezzi (post header)
};

/**
 * Stream di lettura VFS per file aperti da container OBB/APK.
 */
class VfsFileStream {
public:
    VfsFileStream(const ZipEntry& entry, FILE* containerFp);
    ~VfsFileStream();

    // Lettura sequenziale conforme a fread
    size_t read(void* dest, size_t bytes);

    // Riposizionamento conforme a fseek
    int seek(off_t offset, int whence);

    // Posizione corrente conforme a ftell
    off_t tell() const;

    // Dimensione decompressa totale
    size_t size() const { return entry_.uncompressedSize; }

    // Offset di inizio dati nell'archivio (per AAsset_openFileDescriptor)
    off_t dataOffset() const { return entry_.dataPayloadOffset; }

    // File descriptor del container sottostante (per AAsset_openFileDescriptor)
    int getContainerFd() const;

    // Restituisce l'intero buffer decompresso in memoria (per AAsset_getBuffer)
    const uint8_t* getBuffer();

    // Verifica se il file è memorizzato non compresso (STORED)
    bool isStored() const { return entry_.compressionMethod == ZIP_METHOD_STORED; }

private:
    ZipEntry entry_;
    FILE* containerFp_{nullptr};
    bool ownsFp_{false};
    off_t currentPos_{0};

    // Cache per decompressione DEFLATE on-demand
    std::vector<uint8_t> cachedDecompressedData_;
    bool isDecompressed_{false};

    bool decompressFull();
};

/**
 * Virtual File System per container APK e OBB.
 * Fornisce accesso trasparente ai file di gioco compressi e non compressi,
 * con supporto allo streaming $O(1)$ per file STORED e decompressione zlib per file DEFLATED.
 */
class ZipArchiveVfs {
public:
    static ZipArchiveVfs& getInstance();

    // Resetta lo stato di tutti i container montati
    void reset();

    // Monta un archivio APK o OBB e ne indicizza la Central Directory
    bool mountArchive(const std::string& archivePath, const std::string& prefix = "");

    // Cerca automaticamente e monta tutti i file .obb validi in una data cartella
    size_t mountObbDirectory(const std::string& obbDir);

    // Verifica se un file esiste all'interno di uno dei container montati
    bool hasFile(const std::string& relativePath) const;

    // Trova i metadati della ZipEntry corrispondente
    bool findEntry(const std::string& relativePath, ZipEntry& outEntry) const;

    // Apre uno stream virtuale di lettura per il file richiesto
    std::unique_ptr<VfsFileStream> openStream(const std::string& relativePath);

    // Elenca i nomi dei file contenuti in una specifica sotto-directory interna
    std::vector<std::string> listDirectory(const std::string& dirPath) const;

    // Restituisce il numero totale di file indicizzati nel VFS
    size_t getTotalIndexedFiles() const;

private:
    ZipArchiveVfs() = default;
    ~ZipArchiveVfs() = default;
    ZipArchiveVfs(const ZipArchiveVfs&) = delete;
    ZipArchiveVfs& operator=(const ZipArchiveVfs&) = delete;

    // Normalizza un percorso interno (rimuove barre iniziali, converte slash, lowercase opzionale)
    static std::string normalizePath(const std::string& path);

    // Parsing della Central Directory standard ZIP
    bool parseCentralDirectory(const std::string& archivePath, FILE* fp, const std::string& prefix);

    mutable std::mutex vfsMutex_;
    std::unordered_map<std::string, ZipEntry> entries_;
    std::vector<std::string> mountedArchives_;
};

} // namespace emux
