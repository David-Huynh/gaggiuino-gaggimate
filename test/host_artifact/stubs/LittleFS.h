#pragma once
#include <Arduino.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

// In-memory filesystem adapter. Production codecs, recovery and atomic writes
// are compiled unchanged; only the device's filesystem API is replaced.
struct MemoryNode {
    bool directory = false;
    std::vector<uint8_t> bytes;
};
class File : public Stream {
  public:
    std::shared_ptr<MemoryNode> node;
    std::string path;
    size_t cursor = 0, entryCursor = 0;
    std::vector<std::string> entries;
    explicit operator bool() const { return bool(node); }
    bool isDirectory() const { return node && node->directory; }
    size_t size() const { return node ? node->bytes.size() : 0; }
    const char *name() const { return path.c_str(); }
    void close() { node.reset(); }
    int available() override { return node ? int(size() - cursor) : 0; }
    int read() override { return available() ? node->bytes[cursor++] : -1; }
    int peek() override { return available() ? node->bytes[cursor] : -1; }
    void flush() override {}
    bool seek(size_t offset) { if (offset > size()) return false; cursor = offset; return true; }
    size_t read(uint8_t *out, size_t count) {
        count = std::min(count, size() - cursor);
        std::copy_n(node->bytes.data() + cursor, count, out);
        cursor += count;
        return count;
    }
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t *data, size_t count) override {
        if (!node) return 0;
        node->bytes.resize(std::max(size(), cursor + count));
        std::copy_n(data, count, node->bytes.data() + cursor);
        cursor += count;
        return count;
    }
    File openNextFile();
};
class MemoryFS {
  public:
    std::map<std::string, std::shared_ptr<MemoryNode>> nodes;
    bool exists(const String &path) const { return nodes.count(path.c_str()); }
    bool mkdir(const String &path) {
        auto node = std::make_shared<MemoryNode>();
        node->directory = true;
        nodes[path.c_str()] = node;
        return true;
    }
    bool remove(const String &path) { return nodes.erase(path.c_str()) != 0; }
    bool rename(const String &from, const String &to) {
        auto found = nodes.find(from.c_str());
        if (found == nodes.end() || exists(to)) return false;
        nodes[to.c_str()] = found->second;
        nodes.erase(found);
        return true;
    }
    File open(const String &path, const char *mode = FILE_READ) {
        auto found = nodes.find(path.c_str());
        if (mode[0] == 'w' || (mode[0] == 'a' && found == nodes.end())) {
            nodes[path.c_str()] = std::make_shared<MemoryNode>();
            found = nodes.find(path.c_str());
        }
        if (found == nodes.end()) return {};
        File file;
        file.node = found->second;
        file.path = path.c_str();
        if (mode[0] == 'a') file.cursor = file.size();
        if (file.isDirectory()) {
            const auto prefix = file.path + '/';
            for (const auto &entry : nodes)
                if (entry.first.rfind(prefix, 0) == 0 && entry.first.find('/', prefix.size()) == std::string::npos)
                    file.entries.push_back(entry.first);
        }
        return file;
    }
};
inline MemoryFS LittleFS;
inline File File::openNextFile() {
    return entryCursor < entries.size() ? LittleFS.open(entries[entryCursor++].c_str()) : File{};
}
