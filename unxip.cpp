#include <stdio.h>
#include <cstdint>
#include <cstring>
#include <byteswap.h>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/xmlIO.h>
#include <libxml/xinclude.h>
#include <libxml/tree.h>

#define MAX_TOC_SIZE 65535

struct __attribute__((__packed__)) XipHeader {
  uint32_t signature;
  uint16_t header_size;
  uint16_t xar_version;
  uint64_t toc_size_compressed;
  uint64_t toc_size_uncompressed;
  uint32_t checksum_algo;
};

struct XarFile {
  std::string name;
  uint64_t offset;
  uint64_t size;
};

bool ZlibUncompress(uint8_t* in,
                    size_t in_size,
                    uint8_t* out,
                    size_t out_size) {
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;

  strm.next_in = in;
  strm.avail_in = in_size;
  strm.next_out = out;
  strm.avail_out = out_size;

  int error_code = inflateInit(&strm);
  if (error_code != Z_OK) {
    return false;
  }

  error_code = inflate(&strm, Z_NO_FLUSH);
  if (error_code != Z_STREAM_END) {
    return false;
  }

  error_code = inflateEnd(&strm);
  return error_code == Z_OK;
}

std::string CreateXipDirectory(const char* xip_path) {
  // Extract the new directory name based on the XIP path
  std::string dirpath = basename(xip_path);
  size_t ext_pos = dirpath.find_last_of(".");
  if (ext_pos != std::string::npos) {
    dirpath = dirpath.substr(0, ext_pos);
  } else {
    dirpath += "_extracted";
  }
  dirpath += "/";

  mkdir(dirpath.c_str(), 0770);
  return dirpath;
}

bool SaveToFile(const std::string& filepath, uint8_t* data, size_t data_size) {
  FILE* file = fopen(filepath.c_str(), "wb+");
  if (!file) {
    fprintf(stderr, "Error: File \"%s\" can't be created\n", filepath.c_str());
    return false;
  }

  fwrite(data, 1, data_size, file);
  fclose(file);
  return true;
}

bool SaveFileStreamToFile(const std::string& filepath,
                          FILE* file_stream,
                          size_t offset,
                          size_t size) {
  FILE* out_stream = fopen(filepath.c_str(), "wb+");
  if (!out_stream) {
    fprintf(stderr, "Error: File \"%s\" can't be created\n", filepath.c_str());
    return false;
  }

  if (fseek(file_stream, offset, SEEK_SET)) {
    fprintf(stderr, "Invalid file offset\n");
    return false;
  }

  char* buffer[2048];
  size_t size_to_read = sizeof(buffer);
  
  while (size) {
    if (size_to_read > size) {
      size_to_read = size;
    }

    int nb_read = fread(buffer, 1, size_to_read, file_stream);
    if (nb_read != size_to_read) {
      break; // EOF or error
    }

    fwrite(buffer, 1, nb_read, out_stream);
    size -= nb_read;
  }

  fclose(out_stream);
  return true;
}

bool ParseNodeFile(xmlNode* file_node, struct XarFile* xar_file) {
  /*
    <file id="2">
      <data>
        <offset>8111880836</offset>
        <size>355</size>
      </data>
      <name>Metadata</name>
    </file>
  */
  bool is_name_init = false;
  bool is_offset_init = false;
  bool is_size_init = false;

  xmlNode* data_node = nullptr;
  xmlNode* node = file_node->children;
  while (node) {
    if (!node->children) {
      node = node->next;
      continue;
    }
    const char* node_name = reinterpret_cast<const char*>(node->name);
    auto* node_content = reinterpret_cast<const char*>(node->children->content);

    if (!strcmp(node_name, "name")) {
      xar_file->name = std::string(node_content);
      is_name_init = true;
    } else if (!strcmp(node_name, "data")) {
      data_node = node;
    }
    node = node->next;
  }

  if (!is_name_init || !data_node) {
    return false;
  }
  node = data_node->children;
  while (node) {
    if (!node->children) {
      node = node->next;
      continue;
    }
    const char* node_name = reinterpret_cast<const char*>(node->name);
    auto* node_content = reinterpret_cast<const char*>(node->children->content);

    if (!strcmp(node_name, "offset")) {
      xar_file->offset = atol(node_content);
      is_offset_init = true;
    } else if (!strcmp(node_name, "length")) {
      xar_file->size = atol(node_content);
      is_size_init = true;
    }
    node = node->next;
  }

  return is_offset_init && is_size_init;
}

std::vector<struct XarFile> ParseTableOfContents(const uint8_t* toc_xml,
                                                 size_t toc_xml_size) {
  /*
    <xar>
      <toc>
        <file id="1">...</file>
        <file id="2">...</file>
      </toc>
    </xar>
  */
  std::vector<struct XarFile> result;
  xmlDocPtr doc = xmlReadMemory(reinterpret_cast<const char*>(toc_xml),
                                toc_xml_size, "", nullptr, 0);
  if (!doc) {
    fprintf(stderr, "Error: Can't parse the XML from the Table of Contents\n");
    return result;
  }

  xmlNode* xar_node = doc->children;
  xmlNode* toc_node = xar_node->children;

  // Search the <file> elements
  xmlNode* node = toc_node->children;
  struct XarFile xar_file;
  while (node) {
    const char* node_name = reinterpret_cast<const char*>(node->name);

    if (!strcmp(node_name, "file") && ParseNodeFile(node, &xar_file)) {
      result.push_back(xar_file);
    }
    node = node->next;
  }

  xmlFreeDoc(doc);
  return result;
}

int InvalidXipFile(FILE* file) {
  fprintf(stderr, "Error: Invalid XIP file\n");
  fclose(file);
  return 1;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <file.xip>\n", argv[0]);
    return 1;
  }
  char* filepath = argv[1];

  FILE* file = fopen(filepath, "rb");
  if (!file) {
    fprintf(stderr, "Error: File \"%s\" can't be opened\n", filepath);
    return 1;
  }

  std::string outdir = CreateXipDirectory(filepath);

  // Check the XIP headers
  struct XipHeader header;
  memset(&header, 0, sizeof(header));
  size_t nb_read = 0;
  nb_read = fread(&header, 1, sizeof(header), file);

  if (nb_read < 4 || memcmp(&header.signature, "xar!", 4)) {
    fprintf(stderr, "Invalid XAR signature\n");
    return InvalidXipFile(file);
  }

  // Big endian to little endian
  header.header_size = __bswap_16(header.header_size);
  header.xar_version = __bswap_16(header.xar_version);
  header.toc_size_compressed = __bswap_64(header.toc_size_compressed);
  header.toc_size_uncompressed = __bswap_64(header.toc_size_uncompressed);
  header.checksum_algo = __bswap_32(header.checksum_algo);

  if (fseek(file, header.header_size, SEEK_SET)) {
    fprintf(stderr, "Header size is invalid\n");
    return InvalidXipFile(file);
  }

  if (!header.toc_size_compressed ||
      header.toc_size_compressed > MAX_TOC_SIZE ||
      header.toc_size_uncompressed > MAX_TOC_SIZE) {
    fprintf(stderr, "Table of Contents invalid size\n");
    return InvalidXipFile(file);
  }

  // Read the Table of Contents (TOC)
  uint8_t* toc_compressed = new uint8_t[header.toc_size_compressed];
  nb_read = fread(toc_compressed, 1, header.toc_size_compressed, file);
  if (nb_read < header.toc_size_compressed) {
    fprintf(stderr, "Can't read the Table of Contents\n");
    return InvalidXipFile(file);
  }

  const size_t toc_size = header.toc_size_uncompressed;
  uint8_t* toc = new uint8_t[toc_size];
  bool success =
      ZlibUncompress(toc_compressed, header.toc_size_compressed, toc, toc_size);
  if (!success) {
    fprintf(stderr, "Can't uncompress the Table of Contents\n");
    return InvalidXipFile(file);
  }
  delete[] toc_compressed;

  SaveToFile(outdir + "xip_toc.xml", toc, toc_size);

  // Parse the TOC to get the files
  auto files = ParseTableOfContents(toc, toc_size);
  std::string savepath;
  for (const auto& xar_file : files) {
    savepath = outdir + xar_file.name;
    printf("%s\t", savepath.c_str());
    fflush(stdout);

    const size_t offset =
        header.header_size + header.toc_size_compressed + xar_file.offset;
    success = SaveFileStreamToFile(savepath, file, offset, xar_file.size);

    if (success) {
      printf("done\n");
    } else {
      printf("error\n");
    }
  }

  fclose(file);
  return 0;
}
