/* SPDX-License-Identifier: GPL-2.0-only */
/* This backend is a private browser cache, but session cookies are currently
 * RAM-only. Do not let authenticated/session responses outlive that session.
 * Vary needs request-header keys beyond the worker's fixed identity encoding. */
static bool c16_session_url(nsurl *url)
{
    char *cookie=urldb_get_cookie(url,true);
    lwc_string *user=nsurl_get_component(url,NSURL_USERNAME);
    bool session=cookie!=NULL||user!=NULL;
    free(cookie);
    if(user)lwc_string_unref(user);
    return session;
}
static bool c16_persistable(llcache_object *object,long lifetime)
{
    return !object->cache.no_store&&!object->fetch.tainted_tls&&
        object->fetch.post==NULL&&object->fetch.state==LLCACHE_FETCH_COMPLETE&&
        !c16_session_url(object->url)&&
        (lifetime>0||object->cache.etag!=NULL||object->cache.last_modified!=0);
}
