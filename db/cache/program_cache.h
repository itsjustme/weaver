/*
 * ===============================================================
 *    Description:  Cache for user query results
 *
 *        Created:  12/04/2012 09:58:54 PM
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#ifndef __CACHE__
#define __CACHE__

#include <assert.h>
#include <stdint.h>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <po6/net/location.h>
#include <po6/threads/mutex.h>

#include "common/property.h"

namespace cache
{
    typedef std::unordered_map<uint64_t, node_prog::CacheValueBase*> req_map;
    typedef std::unordered_map<uint64_t, req_map> node_map;
    typedef std::unordered_map<node_prog::prog_type, node_map> prog_map;
    typedef std::unordered_map<uint64_t, std::pair<node_prog::prog_type, std::vector<uint64_t>>> invalid_map;

    class program_cache
    {
        prog_map prog_cache, transient_prog_cache;
        invalid_map itable, transient_itable;
        po6::threads::mutex mutex;

        public:
            program_cache();

        public:
            bool cache_exists(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id);
            node_prog::CacheValueBase* single_get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id);
            std::vector<node_prog::CacheValueBase*> get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, 
                std::vector<uint64_t> *dirty_list_ptr, std::unordered_set<uint64_t> &ignore_set);
            void put_cache(uint64_t req_id, node_prog::prog_type t, uint64_t node_handle, node_prog::CacheValueBase *new_cache);
            void delete_cache(uint64_t req_id);
            void commit(uint64_t id);

        private:
            bool entry_exists(node_prog::prog_type t, uint64_t node_handle, prog_map &pc);
            bool cache_exists(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, prog_map &pc);
            node_prog::CacheValueBase* single_get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, prog_map &pc);
            std::vector<node_prog::CacheValueBase*> get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, 
                std::vector<uint64_t> *dirty_list_ptr, std::unordered_set<uint64_t> &ignore_set, prog_map &pc);
            void put_cache(uint64_t req_id, node_prog::prog_type t, uint64_t node_handle, node_prog::CacheValueBase *new_cache, prog_map &pc, invalid_map &it);
            void delete_cache(uint64_t req_id, prog_map &pc, invalid_map &it);
            void remove_entry(uint64_t req_id, prog_map &pc, invalid_map &it);
    };

    inline
    program_cache :: program_cache()
    {
        node_map new_node_map;
        prog_cache.emplace(node_prog::REACHABILITY, new_node_map);
        prog_cache.emplace(node_prog::DIJKSTRA, new_node_map);
        prog_cache.emplace(node_prog::CLUSTERING, new_node_map);
        transient_prog_cache.emplace(node_prog::REACHABILITY, new_node_map);
        transient_prog_cache.emplace(node_prog::DIJKSTRA, new_node_map);
        transient_prog_cache.emplace(node_prog::CLUSTERING, new_node_map);
    }

    inline bool
    program_cache :: cache_exists(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id)
    {
        bool ret;
        mutex.lock();
        ret = cache_exists(t, node_handle, req_id, prog_cache);
        mutex.unlock();
        return ret;
    }

    inline node_prog::CacheValueBase*
    program_cache :: single_get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id)
    {
        node_prog::CacheValueBase *ret;
        mutex.lock();
        ret = single_get_cache(t, node_handle, req_id, prog_cache);
        mutex.unlock();
        return ret;
    }

    inline std::vector<node_prog::CacheValueBase*>
    program_cache :: get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, 
        std::vector<uint64_t> *dirty_list_ptr, std::unordered_set<uint64_t> &ignore_set)
    {
        std::vector<node_prog::CacheValueBase*> ret;
        mutex.lock();
        ret = get_cache(t, node_handle, req_id, dirty_list_ptr, ignore_set, prog_cache);
        mutex.unlock();
        return ret;
    }

    inline void
    program_cache :: put_cache(uint64_t req_id, node_prog::prog_type t, uint64_t node_handle, node_prog::CacheValueBase *new_cache)
    {
        mutex.lock();
        put_cache(req_id, t, node_handle, new_cache, transient_prog_cache, transient_itable);
        put_cache(req_id, t, node_handle, new_cache, prog_cache, itable);
        mutex.unlock();
    }

    inline void
    program_cache :: delete_cache(uint64_t req_id)
    {
        mutex.lock();
        delete_cache(req_id, prog_cache, itable);
        mutex.unlock();
    }

    inline bool
    program_cache :: entry_exists(node_prog::prog_type t, uint64_t node_handle, prog_map &pc)
    {
        node_map &nmap = pc.at(t);
        node_map::iterator nmap_iter = nmap.find(node_handle);
        return (nmap_iter != nmap.end());
    }

    inline bool
    program_cache :: cache_exists(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, prog_map &pc)
    {
        if (entry_exists(t, node_handle, pc)) {
            return (pc.at(t).at(node_handle).find(req_id) != pc.at(t).at(node_handle).end());
        } else {
            return false;
        }
    }

    inline node_prog::CacheValueBase*
    program_cache :: single_get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, prog_map &pc)
    {
        return pc.at(t).at(node_handle).at(req_id);
    }

    inline std::vector<node_prog::CacheValueBase*>
    program_cache :: get_cache(node_prog::prog_type t, uint64_t node_handle, uint64_t req_id, 
        std::vector<uint64_t> *dirty_list_ptr, std::unordered_set<uint64_t> &ignore_set, prog_map &pc)
    {
        std::vector<node_prog::CacheValueBase*> cache;
        if (entry_exists(t, node_handle, pc)) {
            for (auto &entry: pc.at(t).at(node_handle)) {
                // check if entry is for later request or in ignore set
                if ((entry.first < req_id) && (ignore_set.find(entry.first) == ignore_set.end())) {
                    cache.push_back(entry.second);
                    entry.second->set_dirty_list_ptr(dirty_list_ptr);
                }
            }
        }
        return std::move(cache);
    }

    inline void
    program_cache :: put_cache(uint64_t req_id, node_prog::prog_type t, uint64_t node_handle, node_prog::CacheValueBase *new_cache, prog_map &pc, invalid_map &it)
    {
        if (pc.at(t).find(node_handle) == pc.at(t).end()) {
            pc.at(t).emplace(node_handle, req_map());
        }
        pc.at(t).at(node_handle).emplace(req_id, new_cache);
        if (it.find(req_id) == it.end()) {
            it.emplace(req_id, std::make_pair(t, std::vector<uint64_t>()));
        }
        it.at(req_id).second.push_back(node_handle);
    }

    inline void
    program_cache :: delete_cache(uint64_t req_id, prog_map &pc, invalid_map &it)
    {
        // Deleting the cache corresponding to this req_id still leaves
        // the req_id in the list of cached_ids at the other nodes.
        // If and when those nodes are deleted, trying to delete this req_id
        // again would be a no-op.
        if (it.find(req_id) != it.end()) {
            std::pair<node_prog::prog_type, std::vector<uint64_t>> &inv = it.at(req_id);
            for (auto &node: inv.second) {
                node_map &nmap = pc.at(inv.first);
                if (nmap.find(node) != nmap.end()) {
                    req_map &rmap = nmap.at(node);
                    if (rmap.find(req_id) != rmap.end()) {
                        delete rmap.at(req_id);
                        rmap.erase(req_id);
                    }
                    if (rmap.size() == 0) {
                        nmap.erase(node);
                    }
                }
            }
            it.erase(req_id);
        }
    }

    inline void
    program_cache :: remove_entry(uint64_t req_id, prog_map &pc, invalid_map &it)
    {
        // Deleting the cache corresponding to this req_id still leaves
        // the req_id in the list of cached_ids at the other nodes.
        // If and when those nodes are deleted, trying to delete this req_id
        // again would be a no-op.
        if (it.find(req_id) != it.end()) {
            std::pair<node_prog::prog_type, std::vector<uint64_t>> &inv = it.at(req_id);
            for (auto &node: inv.second) {
                node_map &nmap = pc.at(inv.first);
                if (nmap.find(node) != nmap.end()) {
                    req_map &rmap = nmap.at(node);
                    if (rmap.find(req_id) != rmap.end()) {
                        rmap.erase(req_id);
                    }
                    if (rmap.size() == 0) {
                        nmap.erase(node);
                    }
                }
                if (nmap.size() == 0) {
                    pc.erase(inv.first);
                }
            }
            it.erase(req_id);
        }
    }

    inline void
    program_cache :: commit(uint64_t id)
    {
        invalid_map::iterator it;
        mutex.lock();
        it = transient_itable.find(id);
        if (it != transient_itable.end()) {
            // inserting into cache
            std::vector<uint64_t> &nodes = it->second.second;
            for (uint64_t node: nodes) {
                put_cache(id, it->second.first, node, transient_prog_cache.at(it->second.first).at(node).at(id), prog_cache, itable);
            }
            remove_entry(id, transient_prog_cache, transient_itable);
        }
        mutex.unlock();
    }
}

#endif //__CACHE__
