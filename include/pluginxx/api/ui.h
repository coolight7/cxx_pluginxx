/// Declarative UI table: entries (entry / menu / audio effect / selectable style),
/// content (a UI JSON generated on demand and stored on a slot) and slots.
///
/// The host renders from its own cache and never calls a plugin synchronously while
/// drawing: with no content an insert slot draws nothing, a replace slot draws the
/// built-in UI, and `UISlot` draws its `fallback`.
///
/// - an **entry** carries data only (no UI tree): what it looks like is the host's job;
/// - **content** is registered for a slot with a generate function; the host asks for it
///   when the slot is used, when the plugin asks for a refresh, or when a variable changes;
/// - a plugin may **declare its own slot** (`plugin.<pluginId>.ui.<name>`) so other plugins
///   can put content there.
///
/// Slot ids and variable ids are project data: the host injects its own catalog
/// (`pluginxx/host/ui_slots.h`), this header only fixes the wire shape.
///
/// The IID follows the shared table convention (`PLUGINXX_APP_ID("ui")` → `<app>.ui`).
#ifndef PLUGINXX_API_UI_H
#define PLUGINXX_API_UI_H

#include "pluginxx/api/abi.h"
#include "pluginxx/api/app.h"
#include "pluginxx/api/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

#define PLUGINXX_IFACE_UI PLUGINXX_APP_ID("ui")
/// v3: content and slots split from entries (dropped `update_entry`)
/// v4: members and the generate function use the async operation contract
#define PLUGINXX_IFACE_UI_VERSION 4
#define PLUGINXX_IFACE_UI_MIN_VERSION 4
/* Baseline: equals the full layout when reset; afterwards it stays at the offset of the
   first appended member. */
#define PLUGINXX_IFACE_UI_BASELINE_SIZE sizeof(PluginxxUiIface)

/* UI item actions (`data.action`); the same wording as the UI description layer. */
/// Open a declarative plugin page: {"kind":"route","route":"ext://<pluginId>/<contentId>"}
#define PLUGINXX_UI_ACTION_ROUTE "route"
/// Call a capability: {"kind":"call","name":"<name>","args":{...},"timeoutMs":?}
/// A plain string `"<name>"` means the same thing.
#define PLUGINXX_UI_ACTION_CALL "call"
/// No action (display only).
#define PLUGINXX_UI_ACTION_NONE "none"

/// Entry registration spec (entry / menu / audio effect / selectable style metadata).
///
/// `item_id` may be a short name in the plugin namespace (the framework adds
/// `plugin.<pluginId>.`) or a full name that belongs to this plugin.
/// `data_json` is the type specific payload (a JSON object); UI content does not go here,
/// register content (`register_content`) when you need to draw something.
typedef struct PluginxxUiEntrySpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(PluginxxUiEntrySpec)

    PluginxxStringView item_id;   ///< "card" or "plugin.ad_skipper.card"
    PluginxxStringView type;      ///< slot id from the host catalog
    PluginxxStringView data_json; ///< JSON object

    int32_t order;  ///< sort weight (smaller first; equal weights keep registration order)
    int32_t flags;  ///< reserved (must be 0)
} PluginxxUiEntrySpec;

/// Registration spec of one piece of content.
///
/// The generate function turns the declared variables into UI JSON: it receives `context`
/// (a JSON object keyed by the variables listed in `deps`) and answers
/// `{"json": <ui json>, "data": <any json, kept as is>}`.
/// It must be fast: to wait for the network or a file, return a placeholder first and call
/// `push_content` once the data arrives.
///
/// `title` / `depict` are for humans: a selectable slot has to list its candidates before
/// any content exists, so the name must be given at registration time.
typedef struct PluginxxUiContentSpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(PluginxxUiContentSpec)

    PluginxxStringView id;        ///< short id (unique per plugin and slot)
    PluginxxStringView slot;      ///< full or short slot id; may be plugin.<self>.ui.<name>
    /// Display name (used by selectable slots before the content exists; empty = the id)
    PluginxxStringView title;
    /// One line description (subtitle in a selectable slot; may be empty)
    PluginxxStringView depict;
    /// Page routes this content applies to (JSON array; only meaningful for page backgrounds)
    ///
    /// It is needed at registration: the client picks "which one to draw for this page"
    /// before any content exists. Empty or missing = every ordinary page.
    PluginxxStringView routes_json;
    PluginxxStringView deps_json; ///< JSON array of variable ids (only these are subscribed and passed in)
    int32_t            order;     ///< sort weight (smaller first)
    int32_t min_interval_ms;      ///< 0 = no time limit; > 0 = minimum gap between two generations
    int32_t            flags;     ///< reserved (must be 0)

    /// Generate function: `ctx_json` is the context object; answer exactly once via `notify`
    ///
    /// - success: `notify->done(host_ud, PLUGINXX_OPERATOR_OK, {"json":…,"data":…})`;
    /// - it may finish synchronously or later (coroutine / async request);
    /// - failure: `notify->done(host_ud, PLUGINXX_OPERATOR_FAILED, "reason")`
    ///   (the host keeps the last valid content and writes a log line);
    /// - it must be fast: the host does not stop drawing to wait for it.
    ///
    /// The completion protocol is the one of capability handlers: argument order
    /// `(host, ctx, user_data, notify, error_out)`.
    void(PLUGINXX_CALL *generate)(
        const PluginxxHost*           host,
        const PluginxxStringView*     ctx_json,
        void*                         user_data,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );
    void* user_data; ///< context for the generate function (may be null)
} PluginxxUiContentSpec;

/// A slot declared by a plugin (only `insert` / `replace` are allowed).
typedef struct PluginxxUiSlotSpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(PluginxxUiSlotSpec)

    PluginxxStringView id;   ///< plugin.<self>.ui.<name>
    PluginxxStringView kind; ///< "insert" / "replace"
    int32_t            flags; ///< reserved (must be 0)
} PluginxxUiSlotSpec;

typedef struct PluginxxUiIface {
    int32_t  version;     ///< the host fills PLUGINXX_IFACE_UI_VERSION
    uint32_t struct_size;

    /* ---- entries ---- */

    /// Register / replace an entry.
    ///
    /// On completion: `status = OK` with no result payload; on failure the text says why
    /// (`arg` / `state` / `unknown_type` / `namespace`).
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_entry)(
        const PluginxxHost*       host,
        const PluginxxUiEntrySpec* spec,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// Unregister an entry (it must belong to this plugin; missing one fails with `not_found`).
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_entry)(
        const PluginxxHost*       host,
        const PluginxxStringView* item_id,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// List the entries registered by this plugin (JSON array).
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_entries)(
        const PluginxxHost*       host,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /* ---- content ---- */

    /// Register / replace one piece of content.
    /// - registering the same (slot, short id) again replaces that entry (deps / order /
    ///   generate function are replaced too);
    /// - on completion: `status = OK` with no result payload; on failure the text says why
    ///   (`arg` / `state` / `unknown_slot` / `namespace`).
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_content)(
        const PluginxxHost*         host,
        const PluginxxUiContentSpec* spec,
        PluginxxOperatorCallback    cb,
        void*                       cb_ud,
        PluginxxString*             error_out
    );

    /// Unregister one piece of content (short id or full name; missing one fails with `not_found`).
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_content)(
        const PluginxxHost*       host,
        const PluginxxStringView* content_id,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// Publish content right away instead of waiting for the host to ask:
    /// `content_json = {"json":…,"data":…}`.
    ///
    /// The framework only enqueues it (no callback into the plugin, no reentrancy);
    /// the version counter advances like it does for a generated result.
    PluginxxOperatorHandle*(PLUGINXX_CALL* push_content)(
        const PluginxxHost*       host,
        const PluginxxStringView* content_id,
        const PluginxxStringView* content_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// List the content registered by this plugin (JSON array: slot / id / deps / order).
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_contents)(
        const PluginxxHost*       host,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// Ask the host to generate again with the current variables (works even when the slot
    /// is not mounted).
    ///
    /// - `slot`: slot id (short or full);
    /// - `ids_json`: array of this plugin's short content ids; empty / `[]` = every content
    ///   this plugin has on that slot.
    /// The result reaches the host through the content event of the project.
    PluginxxOperatorHandle*(PLUGINXX_CALL* update_slot)(
        const PluginxxHost*       host,
        const PluginxxStringView* slot,
        const PluginxxStringView* ids_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /* ---- slots declared by plugins ---- */

    /// Declare a slot (id must be `plugin.<self>.ui.<name>`; kind may only be insert / replace).
    PluginxxOperatorHandle*(PLUGINXX_CALL* define_slot)(
        const PluginxxHost*      host,
        const PluginxxUiSlotSpec* spec,
        PluginxxOperatorCallback cb,
        void*                    cb_ud,
        PluginxxString*          error_out
    );

    /// Withdraw a slot declared by this plugin (content on it stops working as well).
    PluginxxOperatorHandle*(PLUGINXX_CALL* undefine_slot)(
        const PluginxxHost*       host,
        const PluginxxStringView* slot,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// List the slot table: the ones the host reserves plus the ones plugins declared.
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_slots)(
        const PluginxxHost*       host,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /* ---- notifications ---- */

    /// Show a message (equivalent to the toast capability; fire and forget).
    PluginxxOperatorHandle*(PLUGINXX_CALL* notify)(
        const PluginxxHost*       host,
        const PluginxxStringView* message_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );
} PluginxxUiIface;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* PLUGINXX_API_UI_H */
