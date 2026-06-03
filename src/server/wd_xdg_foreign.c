#include "wd_server.h"

bool wd_xdg_foreign_init(struct wd_server* server) {
    if (!server || !server->display || !server->xdg_shell)
    {
        return false;
    }

    /*
     * xdg-foreign v2 lets one client export an xdg_toplevel handle and another
     * client import it, then use that imported handle as the parent for its own
     * toplevel. This is how portal/file-picker style out-of-process dialogs
     * can associate themselves with the app window that launched them.
     *
     * wlroots' registry wires the xdg-foreign protocol into xdg-shell parent
     * handling. WayDisplay's visible placement policy remains in wd_scene.c:
     * when clients establish the parent relationship, the existing
     * xdg_toplevel.set_parent listener centers/transient-places the child.
     */
    server->xdg_foreign_registry = wlr_xdg_foreign_registry_create(server->display);
    if (!server->xdg_foreign_registry)
    {
        WD_LOG_ERROR("WayDisplay: failed to create xdg-foreign registry");
        return false;
    }

    if (!wlr_xdg_foreign_v2_create(server->display, server->xdg_foreign_registry))
    {
        WD_LOG_ERROR("WayDisplay: failed to create xdg-foreign v2 manager");
        return false;
    }

    WD_LOG_INFO("WayDisplay: xdg-foreign v2 enabled");
    return true;
}

void wd_xdg_foreign_destroy(struct wd_server* server) {
    if (!server)
    {
        return;
    }

    /*
     * wlroots owns the manager globals through the Wayland display. The registry
     * does not need explicit destruction here in wlroots 0.18/0.19 style APIs;
     * clear our pointer so destroy paths do not reuse it.
     */
    server->xdg_foreign_registry = NULL;
}
