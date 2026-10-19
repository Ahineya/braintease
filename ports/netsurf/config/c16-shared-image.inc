/* SPDX-License-Identifier: GPL-2.0-only */
/* A reference supplied explicitly by the owning HTML document, never a URL
 * lookup across documents. Sharing a live image is distinct from HTTP cache
 * reuse: no-store/no-cache still require a request on the next navigation. */
static llcache_handle *c16_shared_image(hlcache_handle *source, nsurl *url,
                                     hlcache_child_context *child)
{
    if (source == NULL || child == NULL ||
        !nsurl_compare(hlcache_handle_get_url(source), url, NSURL_COMPLETE))
        return NULL;
    if (source->entry != NULL) {
        struct content *c = source->entry->content;
        if (content_get_status(source) == CONTENT_STATUS_ERROR ||
            content_get_type(source) != CONTENT_IMAGE ||
            !content_is_shareable(c) || !content_matches_quirks(c, child->quirks))
            return NULL;
        return (llcache_handle *)content_get_llcache_handle(c);
    }
    RING_ITERATE_START(struct hlcache_retrieval_ctx,
                      hlcache->retrieval_ctx_ring, ctx) {
        if (ctx->handle == source && !ctx->migrate_target &&
            ctx->accepted_types == CONTENT_IMAGE &&
            ctx->child.quirks == child->quirks)
            return ctx->llcache;
    } RING_ITERATE_END(hlcache->retrieval_ctx_ring, ctx);
    return NULL;
}
