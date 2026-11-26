/// Declarative UI registry: entries, content and plugin-declared slots.
///
/// The host owns one registry and touches it from its framework thread only (registration,
/// removal, snapshots). Generation and event delivery stay in the host: it decides where the
/// generate function runs and which event it pushes.
///
/// What lives here is the project independent part:
/// - the three record types and their storage, with insertion-order counters (`seq`) so
///   snapshots are stable under equal sort weights;
/// - the result cache of a content (`rev` grows on every generated result);
/// - the snapshot JSON used by the `<app>.ui` table (`id` / `type` / `data` / `slot` / `deps`
///   / `rev` keys are the wire shape);
/// - detaching everything an instance owns when it stops or unloads.
///
/// Namespace and payload validation stay in the host: what a `title` is required for, and
/// which data fields are clamped, is project policy.
#ifndef PLUGINXX_HOST_UI_CONTENT_H
#define PLUGINXX_HOST_UI_CONTENT_H

#include "pluginxx/api/ui.h"
#include "pluginxx/host/ui_slots.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pluginxx {
namespace host {

/// One registered declaration item (lifetime = the plugin instance).
struct UiEntry {
  std::string plugin;    ///< instance name
  std::string pluginId;  ///< plugin id
  std::string id;       ///< full id (plugin.<pluginId>.<name>)
  std::string type;     ///< slot id
  std::string dataJson; ///< declarative payload (JSON object)
  int32_t order = 0;
  uint64_t seq = 0; ///< registration order (keeps equal orders stable)

  /// Redraw counter: only a re-register or an explicit refresh bumps it.
  int32_t rev = 0;
};

/// One piece of content (a UI JSON generated on demand for a slot).
struct UiContent {
  std::string plugin;    ///< instance name
  std::string pluginId;  ///< plugin id
  std::string id;       ///< short id
  std::string fullId;   ///< plugin.<pluginId>.<id>
  std::string slot;     ///< full slot id
  std::string title;    ///< display name (empty = use the id)
  std::string depict;   ///< one line description (may be empty)
  std::string routesJson; ///< page routes (JSON array; only for page backgrounds)
  std::string depsJson;   ///< declared variables (JSON array)
  std::vector<std::string> deps;
  int32_t order = 0;
  int32_t minIntervalMs = 0;
  int32_t rev = 0; ///< result version (every generated result +1)
  uint64_t seq = 0; ///< registration order
  bool fromJs = false;
  PluginxxUiContentSpec spec{}; ///< copy (holds the generate function)

  /// Last generated result (kept so a failure does not blank the UI).
  std::string lastJson;   ///< UI JSON text
  std::string lastData;   ///< optional data JSON text
  bool hasResult = false;
};

/// A slot declared by a plugin (its lifetime follows the plugin instance).
struct UiSlot {
  std::string plugin;
  std::string pluginId;
  std::string id;   ///< plugin.<pluginId>.ui.<name>
  std::string kind; ///< "insert" / "replace"
  uint64_t seq = 0;
};

/// Result of [UiRegistry::detachOwner]: which tables actually changed.
struct UiDetachResult {
  bool entries = false;
  bool contents = false;
  bool slots = false;

  bool any() const { return entries || contents || slots; }
};

class UiRegistry {
public:
  using EntryMap = std::map<std::string, UiEntry, std::less<>>;
  using ContentMap = std::map<std::string, UiContent, std::less<>>;
  using SlotMap = std::map<std::string, UiSlot, std::less<>>;

  EntryMap& entries() { return entries_; }
  const EntryMap& entries() const { return entries_; }
  ContentMap& contents() { return contents_; }
  const ContentMap& contents() const { return contents_; }
  SlotMap& pluginSlots() { return pluginSlots_; }
  const SlotMap& pluginSlots() const { return pluginSlots_; }

  uint64_t nextEntrySeq() { return ++entrySeq_; }
  uint64_t nextContentSeq() { return ++contentSeq_; }
  uint64_t nextSlotSeq() { return ++slotSeq_; }

  UiEntry* findEntry(std::string_view fullId);
  UiContent* findContent(std::string_view fullId);

  /// Content a plugin put on one slot (empty [pluginId] = any owner).
  std::vector<UiContent*> contentsOn(std::string_view pluginId,
                                     std::string_view slot);

  /// Build the payload of one generated result and bump its `rev`.
  ///
  /// Shape: `{"slot","id","shortId","plugin","rev","json"|"error"}`. `jsonText` may be the
  /// wrapper `{"json":…,"data":…}` or a plain UI JSON. [error] non-empty means this attempt
  /// failed; the last good result is kept. Returns an empty string when the content is gone.
  std::string buildContentPayload(std::string_view fullId,
                                  std::string_view jsonText,
                                  std::string_view error);

  /// Snapshot of the declaration items (`[]` when none); sorted by type / order / seq.
  std::string entriesJson(std::string_view pluginId = {}) const;

  /// Snapshot of the content registry; sorted by slot / order / seq.
  std::string contentsJson(std::string_view pluginId = {}) const;

  /// Snapshot of the slot table: the host's reserved slots plus the declared ones.
  std::string slotsJson(const UiSlotCatalog& catalog,
                        std::string_view pluginId = {}) const;

  /// Drop everything [instanceName] owns (called when it stops or unloads).
  UiDetachResult detachOwner(std::string_view instanceName);

private:
  EntryMap entries_;
  ContentMap contents_;
  SlotMap pluginSlots_;
  uint64_t entrySeq_ = 0;
  uint64_t contentSeq_ = 0;
  uint64_t slotSeq_ = 0;
};

} // namespace host
} // namespace pluginxx

#endif // PLUGINXX_HOST_UI_CONTENT_H
