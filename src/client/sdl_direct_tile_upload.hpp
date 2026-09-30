#pragma once

#include "render_planning.hpp"
#include "tile_present_queue.hpp"

#include <vector>

struct SDL_Texture;

namespace waydisplay {

struct ClientState;

bool upload_completed_tiles_direct(ClientState& state, SDL_Texture* texture,
                                   std::vector<ClientTileUpload>& uploads,
                                   std::vector<ClientDirtyRect>& uploaded_rects);
void recycle_direct_tile_upload_buffers(ClientState& state,
                                        std::vector<ClientTileUpload>& uploads);

} // namespace waydisplay
