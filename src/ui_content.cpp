#include "pluginxx/host/ui_content.h"

#include "utilxx_base/json.h"

#include <algorithm>

namespace pluginxx {
namespace host {

namespace {

using utilxx_base::Json;

Json parseSafe(std::string_view text) {
  if (text.empty()) {
    return Json{};
  }
  try {
    return Json::parse(text);
  } catch (...) {
    return Json{};
  }
}

} // namespace

UiEntry* UiRegistry::findEntry(std::string_view fullId) {
  auto it = entries_.find(std::string{fullId});
  return it == entries_.end() ? nullptr : &it->second;
}

UiContent* UiRegistry::findContent(std::string_view fullId) {
  auto it = contents_.find(std::string{fullId});
  return it == contents_.end() ? nullptr : &it->second;
}

std::vector<UiContent*> UiRegistry::contentsOn(std::string_view pluginId,
                                               std::string_view slot) {
  std::vector<UiContent*> out;
  for (auto& [key, content] : contents_) {
    (void)key;
    if ((!pluginId.empty() && content.pluginId != pluginId) ||
        content.slot != slot) {
      continue;
    }
    out.push_back(&content);
  }
  std::sort(out.begin(), out.end(), [](const UiContent* a, const UiContent* b) {
    if (a->order != b->order) {
      return a->order < b->order;
    }
    return a->seq < b->seq;
  });
  return out;
}

std::string UiRegistry::buildContentPayload(std::string_view fullId,
                                           std::string_view jsonText,
                                           std::string_view error) {
  UiContent* content = findContent(fullId);
  if (nullptr == content) {
    return {};
  }
  ++content->rev;
  if (error.empty()) {
    // The generator answers {"json": …, "data": …}; a plain UI JSON is accepted too.
    Json body = parseSafe(jsonText);
    if (body.is_object() && body.contains("json")) {
      content->lastJson = body["json"].dump();
      content->lastData =
          body.contains("data") ? body["data"].dump() : std::string{};
    } else {
      content->lastJson = body.is_null() ? std::string{"null"} : body.dump();
      content->lastData.clear();
    }
    content->hasResult = true;
  }
  Json payload;
  payload["slot"] = content->slot;
  payload["id"] = content->fullId;
  payload["shortId"] = content->id;
  payload["plugin"] = content->pluginId;
  payload["rev"] = content->rev;
  if (!error.empty()) {
    payload["error"] = std::string{error};
  } else if (content->hasResult) {
    payload["json"] = parseSafe(content->lastJson);
    if (!content->lastData.empty()) {
      payload["data"] = parseSafe(content->lastData);
    }
  }
  return payload.dump();
}

std::string UiRegistry::entriesJson(std::string_view pluginId) const {
  std::vector<const UiEntry*> items;
  items.reserve(entries_.size());
  for (const auto& [key, entry] : entries_) {
    (void)key;
    if (pluginId.empty() || entry.pluginId == pluginId) {
      items.push_back(&entry);
    }
  }
  std::sort(items.begin(), items.end(), [](const UiEntry* a, const UiEntry* b) {
    if (a->type != b->type) {
      return a->type < b->type;
    }
    if (a->order != b->order) {
      return a->order < b->order;
    }
    return a->seq < b->seq;
  });
  Json array = Json::array();
  for (const UiEntry* entry : items) {
    Json item;
    item["id"] = entry->id;
    item["plugin"] = entry->pluginId;
    item["type"] = entry->type;
    item["order"] = entry->order;
    item["rev"] = entry->rev;
    item["data"] = parseSafe(entry->dataJson);
    array.push_back(std::move(item));
  }
  return array.dump();
}

std::string UiRegistry::contentsJson(std::string_view pluginId) const {
  std::vector<const UiContent*> items;
  items.reserve(contents_.size());
  for (const auto& [key, content] : contents_) {
    (void)key;
    if (pluginId.empty() || content.pluginId == pluginId) {
      items.push_back(&content);
    }
  }
  std::sort(items.begin(), items.end(),
            [](const UiContent* a, const UiContent* b) {
              if (a->slot != b->slot) {
                return a->slot < b->slot;
              }
              if (a->order != b->order) {
                return a->order < b->order;
              }
              return a->seq < b->seq;
            });
  Json array = Json::array();
  for (const UiContent* content : items) {
    Json item;
    item["id"] = content->fullId;
    item["shortId"] = content->id;
    item["plugin"] = content->pluginId;
    item["slot"] = content->slot;
    // The display name has to be known before the content exists (selectable slots).
    if (!content->title.empty()) {
      item["title"] = content->title;
    }
    if (!content->depict.empty()) {
      item["depict"] = content->depict;
    }
    if (!content->routesJson.empty() && content->routesJson != "[]") {
      item["routes"] = parseSafe(content->routesJson);
    }
    item["order"] = content->order;
    item["minIntervalMs"] = content->minIntervalMs;
    item["deps"] = parseSafe(content->depsJson);
    item["rev"] = content->rev;
    array.push_back(std::move(item));
  }
  return array.dump();
}

std::string UiRegistry::slotsJson(const UiSlotCatalog& catalog,
                                  std::string_view pluginId) const {
  Json array = Json::array();
  for (const UiSlotDecl& slot : catalog.slots()) {
    Json item;
    item["id"] = slot.id ? slot.id : "";
    item["kind"] = slot.kind ? slot.kind : "";
    item["owner"] = "";
    array.push_back(std::move(item));
  }
  for (const auto& [id, slot] : pluginSlots_) {
    if (!pluginId.empty() && slot.pluginId != pluginId) {
      continue;
    }
    Json item;
    item["id"] = slot.id;
    item["kind"] = slot.kind;
    item["owner"] = slot.pluginId;
    array.push_back(std::move(item));
  }
  return array.dump();
}

UiDetachResult UiRegistry::detachOwner(std::string_view instanceName) {
  UiDetachResult result;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.plugin == instanceName) {
      it = entries_.erase(it);
      result.entries = true;
    } else {
      ++it;
    }
  }
  for (auto it = contents_.begin(); it != contents_.end();) {
    if (it->second.plugin == instanceName) {
      it = contents_.erase(it);
      result.contents = true;
    } else {
      ++it;
    }
  }
  for (auto it = pluginSlots_.begin(); it != pluginSlots_.end();) {
    if (it->second.plugin == instanceName) {
      it = pluginSlots_.erase(it);
      result.slots = true;
    } else {
      ++it;
    }
  }
  return result;
}

} // namespace host
} // namespace pluginxx
