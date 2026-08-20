#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>

#include "../BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char itemCacheFile[] = "/.items.bin";
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  if (parser) {
    XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
    XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
    XML_SetCharacterDataHandler(parser, nullptr);
    XML_ParserFree(parser);
    parser = nullptr;
  }
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

bool ContentOpfParser::readItemRecord(std::string& out, const uint32_t expectedLen) {
  uint32_t len = 0;
  if (tempItemStore.read(&len, sizeof(len)) != static_cast<int>(sizeof(len))) {
    return false;
  }
  if (expectedLen != ANY_RECORD_LENGTH && len != expectedLen) {
    return false;
  }
  if (len > static_cast<uint32_t>(tempItemStore.available())) {
    return false;
  }

  out.assign(len, '\0');
  return len == 0 || tempItemStore.read(&out[0], len) == static_cast<int>(len);
}

void ContentOpfParser::indexItemStore() {
  if (!tempItemStore) {
    return;
  }

  itemIndex.reserve(ITEM_INDEX_INITIAL_CAPACITY);
  tempItemStore.seek(0);
  std::string itemId;
  std::string href;
  while (tempItemStore.available()) {
    const auto offset = static_cast<uint32_t>(tempItemStore.position());
    if (!readItemRecord(itemId) || !readItemRecord(href)) {
      LOG_ERR("COF", "Item store is truncated at offset %lu", offset);
      break;
    }
    itemIndex.push_back({fnvHash(itemId), static_cast<uint16_t>(itemId.size()), offset});
  }
  LOG_DBG("COF", "Rebuilt item index from the store with %zu entries", itemIndex.size());
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_MANIFEST;
    // openFileForWrite truncates the store, so anything a previous <manifest> indexed is stale
    self->itemIndex.clear();
    self->itemIndex.reserve(ITEM_INDEX_INITIAL_CAPACITY);
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // The <manifest> pass normally fills the index. If it did not - its write open failed, or an
    // earlier </spine> released the index - recover it from the store rather than resolving nothing.
    if (self->itemIndex.empty()) {
      self->indexItemStore();
    }

    // Sort the index so each <itemref> below can binary search it instead of rescanning .items.bin
    std::sort(self->itemIndex.begin(), self->itemIndex.end(), itemIndexLess);
    LOG_DBG("COF", "Indexed %zu manifest items", self->itemIndex.size());
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    bool isCover = false;
    std::string coverItemId;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
      } else if (strcmp(atts[i], "content") == 0) {
        coverItemId = atts[i + 1];
      }
    }

    if (isCover) {
      self->coverItemId = coverItemId;
    }
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "item") == 0 || strcmp(name, "opf:item") == 0)) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(self->baseContentPath + atts[i + 1]);
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record index entry for fast lookup later
    if (self->tempItemStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->itemIndex.push_back(entry);
    }

    // Write items down to SD card
    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    if (itemId == self->coverItemId) {
      self->coverItemHref = href;
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && (strcmp(name, "itemref") == 0 || strcmp(name, "opf:itemref") == 0)) {
      // Without the temp item store there is nothing to resolve idrefs against. Bail out rather than
      // seeking a closed file: serialization::readString would then resize a std::string to an
      // uninitialised length, which aborts the firmware under -fno-exceptions.
      if (!self->tempItemStore) {
        return;
      }

      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          bool found = false;

          const ItemIndexEntry target{fnvHash(idref), static_cast<uint16_t>(idref.size()), 0};
          auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(), target, itemIndexLess);

          // Entries sharing a hash and length are adjacent, so walk them until the id itself matches.
          // Anything beyond that group differs in hash or length and therefore cannot be this idref.
          while (it != self->itemIndex.end() && it->idHash == target.idHash && it->idLen == target.idLen) {
            // Check the record against the index before trusting the offset. A failed or short SD
            // write during <manifest> can leave later entries pointing at EOF or into the middle of a
            // record, and reading one with serialization::readString would resize a std::string to an
            // uninitialised or bogus length - fatal under -fno-exceptions. The linear scan this
            // replaces was bounded by available(), so it degraded to "not found" instead.
            std::string itemId;
            if (!self->tempItemStore.seek(it->fileOffset) || !self->readItemRecord(itemId, it->idLen)) {
              self->rejectedItemRecords++;
              ++it;
              continue;
            }

            if (itemId == idref) {
              // The id matched, so this is a real record start - but the href that follows it can
              // still be the half of the record a short write dropped, so it is read checked too
              if (self->readItemRecord(href)) {
                found = true;
              } else {
                self->rejectedItemRecords++;
              }
              break;
            }
            ++it;
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && (strcmp(name, "reference") == 0 || strcmp(name, "opf:reference") == 0)) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(self->baseContentPath + atts[i + 1]);
      }
    }
    if (!guideHref.empty()) {
      if (type == "text" || (type == "start" && !self->textReferenceHref.empty())) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      self->author.append(", ");  // Add separator for multiple authors
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    // Reported once rather than per itemref: every log line also lands in the 16-entry RTC ring
    // that crash reports are read back from, so a per-entry error would evict the crash context
    if (self->rejectedItemRecords > 0) {
      LOG_ERR("COF", "Item store held no valid record for %u indexed items", self->rejectedItemRecords);
    }
    // Nothing after the spine looks items up, so hand the index memory back before
    // Epub::parseContentOpf runs its guide cover fallback, which loads whole XHTML files into RAM
    std::vector<ItemIndexEntry>().swap(self->itemIndex);
    return;
  }

  if (self->state == IN_GUIDE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
