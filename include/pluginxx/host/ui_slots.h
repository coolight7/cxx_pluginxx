/// UI slot catalog: the slots and variables a host knows about.
///
/// The table itself is project data (a host declares its slots in its own definition file
/// and generates a catalog from it). This header only fixes the shape so the shared UI
/// registry (`pluginxx/host/ui_content.h`) can normalize ids and validate dependencies
/// without knowing which slots exist.
///
/// Naming:
/// - a slot id is a dotted string; the catalog is built with one `prefix` (for example
///   `musicxx.ui.`) and accepts either the full id or the short form without that prefix;
/// - a variable id is either a catalog entry or lives in a plugin namespace
///   (`plugin.<pluginId>.<name>`).
#ifndef PLUGINXX_HOST_UI_SLOTS_H
#define PLUGINXX_HOST_UI_SLOTS_H

#include <string>
#include <string_view>
#include <vector>

namespace pluginxx {
namespace host {

/// What a slot is for; the host decides how to draw each kind.
enum class UiSlotKind {
  Unknown = 0,
  /// A declaration item (entry / menu item / audio effect / selectable style): data only.
  Item,
  /// Content is inserted into the page, every registered piece is drawn.
  Insert,
  /// Content replaces a built-in widget, only the entry with the smallest order is drawn.
  Replace,
  /// Content is one candidate the user can select (background style, visualizer, ...).
  Selectable,
  /// Full page background chosen by the user.
  PageBackground,
  /// A whole page; content provides the page body.
  Page,
  /// An overlay drawn above the page.
  Overlay,
};

/// Kind of a slot from its text form ("item" / "insert" / "replace" / "selectable" /
/// "pageBackground" / "page" / "overlay"); unknown text maps to `UiSlotKind::Unknown`.
UiSlotKind uiSlotKindOf(std::string_view text);

/// Text form of a slot kind ("item", "insert", ...); `Unknown` maps to "unknown".
std::string_view uiSlotKindName(UiSlotKind kind);

/// One slot the host reserves.
struct UiSlotDecl {
  const char* id = nullptr;   ///< full id
  const char* kind = nullptr; ///< text form of `UiSlotKind`
};

/// Immutable catalog of the slots and variables a host knows.
class UiSlotCatalog {
public:
  UiSlotCatalog() = default;

  /// [prefix] is the host's slot namespace including the trailing dot (`musicxx.ui.`);
  /// [slots] keeps the declaration order, [variables] is the list of known variable ids.
  UiSlotCatalog(std::string_view prefix, std::vector<UiSlotDecl> slots,
                std::vector<std::string> variables);

  /// Slot declaration by full id; null when the id is not in the table.
  const UiSlotDecl* find(std::string_view fullId) const;

  /// Whether [fullId] is one of the slots the host reserves.
  bool contains(std::string_view fullId) const { return find(fullId) != nullptr; }

  /// Short form (without `prefix`) to full id, for ids that exist in the table.
  ///
  /// An id that already has the prefix, a plugin slot (`plugin.*`) or an id the table does
  /// not know is returned unchanged, so a plugin-declared slot passes through.
  std::string fullName(std::string_view raw) const;

  /// Whether [id] is a variable the host knows.
  bool isKnownVariable(std::string_view id) const;

  /// Kind text of a slot (empty when unknown).
  std::string_view kindTextOf(std::string_view fullId) const;

  /// Kind of a slot (`Unknown` when the id is not in the table).
  UiSlotKind kindOf(std::string_view fullId) const {
    return uiSlotKindOf(kindTextOf(fullId));
  }

  const std::vector<UiSlotDecl>& slots() const { return slots_; }

  std::string_view prefix() const { return prefix_; }

private:
  std::string prefix_;
  std::vector<UiSlotDecl> slots_;
  std::vector<std::string> slotIds_; ///< owned copy of the ids (UiSlotDecl points here)
  std::vector<std::string> variables_;
};

} // namespace host
} // namespace pluginxx

#endif // PLUGINXX_HOST_UI_SLOTS_H
