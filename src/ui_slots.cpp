#include "pluginxx/host/ui_slots.h"

#include <algorithm>

namespace pluginxx {
namespace host {

namespace {

struct KindEntry {
  const char* text;
  UiSlotKind kind;
};

constexpr KindEntry kKinds[] = {
    {"item", UiSlotKind::Item},
    {"insert", UiSlotKind::Insert},
    {"replace", UiSlotKind::Replace},
    {"selectable", UiSlotKind::Selectable},
    {"pageBackground", UiSlotKind::PageBackground},
    {"page", UiSlotKind::Page},
    {"overlay", UiSlotKind::Overlay},
};

} // namespace

UiSlotKind uiSlotKindOf(std::string_view text) {
  for (const KindEntry& entry : kKinds) {
    if (text == entry.text) {
      return entry.kind;
    }
  }
  return UiSlotKind::Unknown;
}

std::string_view uiSlotKindName(UiSlotKind kind) {
  for (const KindEntry& entry : kKinds) {
    if (kind == entry.kind) {
      return entry.text;
    }
  }
  return "unknown";
}

UiSlotCatalog::UiSlotCatalog(std::string_view prefix,
                             std::vector<UiSlotDecl> slots,
                             std::vector<std::string> variables)
    : prefix_(prefix), slots_(std::move(slots)),
      variables_(std::move(variables)) {
  // The declarations point at the ids owned here, so the catalog stays valid when the
  // caller's strings go away.
  slotIds_.reserve(slots_.size());
  for (UiSlotDecl& slot : slots_) {
    slotIds_.emplace_back(slot.id ? slot.id : "");
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    slots_[i].id = slotIds_[i].c_str();
  }
}

const UiSlotDecl* UiSlotCatalog::find(std::string_view fullId) const {
  for (const UiSlotDecl& slot : slots_) {
    if (fullId == std::string_view{slot.id ? slot.id : ""}) {
      return &slot;
    }
  }
  return nullptr;
}

std::string UiSlotCatalog::fullName(std::string_view raw) const {
  std::string text{raw};
  if (text.empty() || text.rfind(prefix_, 0) == 0 ||
      text.rfind("plugin.", 0) == 0) {
    return text;
  }
  std::string full = prefix_ + text;
  return contains(full) ? full : text;
}

bool UiSlotCatalog::isKnownVariable(std::string_view id) const {
  for (const std::string& variable : variables_) {
    if (id == variable) {
      return true;
    }
  }
  return false;
}

std::string_view UiSlotCatalog::kindTextOf(std::string_view fullId) const {
  const UiSlotDecl* slot = find(fullId);
  if (nullptr == slot || nullptr == slot->kind) {
    return {};
  }
  return slot->kind;
}

} // namespace host
} // namespace pluginxx
