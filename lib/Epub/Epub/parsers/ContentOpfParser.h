#pragma once
#include <Print.h>

#include <algorithm>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  FsFile tempItemStore;
  std::string coverItemId;

  // Index for fast idref→href lookup, built while parsing <manifest> and consumed while parsing <spine>
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::vector<ItemIndexEntry> itemIndex;

  // Manifest size isn't known until </manifest>, so reserve enough for a typical book up front.
  // Every growth is an allocate/copy/free cycle that fragments DRAM, and the first seven of them
  // (1, 2, 4, ... 64 entries) happen while the manifest is still streaming in.
  static constexpr size_t ITEM_INDEX_INITIAL_CAPACITY = 64;

  // Ordering shared by the sort and the lookup. They must agree exactly, otherwise lower_bound
  // silently misses entries, so keep a single definition rather than two matching lambdas.
  // fileOffset breaks ties so that entries which collide on both hash and length stay in manifest
  // order: std::sort is not stable, and a duplicate id must still resolve to the first item declared.
  static bool itemIndexLess(const ItemIndexEntry& a, const ItemIndexEntry& b) {
    if (a.idHash != b.idHash) return a.idHash < b.idHash;
    if (a.idLen != b.idLen) return a.idLen < b.idLen;
    return a.fileOffset < b.fileOffset;
  }

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
