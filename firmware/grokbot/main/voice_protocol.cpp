#include "voice_transport.h"
#include "cJSON.h"
#include <cstring>

VoiceReply voice_decode(const char *body, const char *request_id)
{
    VoiceReply reply;
    strlcpy(reply.error, "invalid_response", sizeof(reply.error));
    cJSON *root = cJSON_ParseWithOpts(body, nullptr, true);
    if (!root) return reply;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (cJSON_IsString(id) && (!request_id || !strcmp(id->valuestring, request_id)) && cJSON_IsString(status)) {
        const struct { const char *name; VoicePhase phase; } states[] = {
            {"stored", VoicePhase::Stored}, {"transcribing", VoicePhase::Transcribing},
            {"ready", VoicePhase::Waiting}, {"claimed", VoicePhase::Claimed},
            {"replied", VoicePhase::Replied}, {"needs_review", VoicePhase::NeedsReview},
            {"empty", VoicePhase::Idle},
            {"failed", VoicePhase::Failed}
        };
        for (const auto &state : states) {
            if (strcmp(status->valuestring, state.name)) continue;
            const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "page_count");
            const cJSON *read = cJSON_GetObjectItemCaseSensitive(root, "read");
            size_t length = strlen(id->valuestring);
            bool valid_id = length == 32 && strspn(id->valuestring, "0123456789" "abcdef") == 32;
            if (!cJSON_IsNumber(pages) || pages->valuedouble != pages->valueint ||
                pages->valueint < 0 || pages->valueint > VOICE_PAGE_LIMIT || !cJSON_IsBool(read)) break;
            if (state.phase == VoicePhase::Idle ? (request_id || length || pages->valueint || cJSON_IsTrue(read)) : !valid_id) break;
            if (state.phase == VoicePhase::Replied ? !pages->valueint :
                (pages->valueint || cJSON_IsTrue(read))) break;
            if (!request_id && state.phase != VoicePhase::Idle &&
                (state.phase != VoicePhase::Replied || cJSON_IsTrue(read))) break;
            strlcpy(reply.request_id, id->valuestring, sizeof(reply.request_id));
            reply.page_count = pages->valueint;
            reply.read = cJSON_IsTrue(read);
            reply.ok = true;
            reply.phase = state.phase;
            reply.error[0] = 0;
            const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
            if (cJSON_IsString(error)) strlcpy(reply.error, error->valuestring, sizeof(reply.error));
            break;
        }
    }
    cJSON_Delete(root);
    return reply;
}
