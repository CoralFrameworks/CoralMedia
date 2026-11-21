/*
 * Copyright (C) 2026 The Coral Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CORAL_MEDIA_SYNC_TOKEN_D3D11_H
#define CORAL_MEDIA_SYNC_TOKEN_D3D11_H

#include <media/SyncToken.h>

#if defined(_WIN32)

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Query;

namespace coral {

/**
 * Wraps an already-ended D3D11 query as a SyncToken. The query must stay alive for the token's
 * lifetime; the converter uses this to expose the query that fences its copy.
 */
std::shared_ptr<SyncToken> createD3D11QuerySyncTokenFromQuery(ID3D11DeviceContext* context,
  ID3D11Query* query);

/**
 * Creates a query-backed token that is signalled when the D3D11 context reaches the query. The query
 * is ended immediately, so the token represents work already submitted at creation time.
 */
std::shared_ptr<SyncToken> createD3D11QuerySyncToken(ID3D11Device* device, ID3D11DeviceContext* context);

/**
 * Creates a shared D3D11 fence signalled at @p value. Devices without `ID3D11Device5` fall back to a
 * query token.
 */
std::shared_ptr<SyncToken> createD3D11FenceSyncToken(ID3D11Device* device,
  ID3D11DeviceContext* context, uint64_t value = 1);

} // namespace coral

#endif // defined(_WIN32)

#endif // CORAL_MEDIA_SYNC_TOKEN_D3D11_H
