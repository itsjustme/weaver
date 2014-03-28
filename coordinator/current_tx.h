/*
 * ===============================================================
 *    Description:  DS to keep track of transaction replies from
 *                  shards.
 *
 *        Created:  2014-02-24 12:20:10
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013-2014, Cornell University, see the LICENSE
 *                     file for licensing agreement
 * ===============================================================
 */

#ifndef weaver_coordinator_current_tx_h_
#define weaver_coordinator_current_tx_h_

namespace coordinator
{
    struct current_tx
    {
        uint64_t id;
        uint64_t client;
        uint64_t count;
        vc::vclock timestamp;
        std::shared_ptr<std::vector<transaction::pending_tx>> tx_vec;

        current_tx() : client(UINT64_MAX), count(0) { }
        current_tx(uint64_t _id, uint64_t cl, vc::vclock &vclk,
            std::shared_ptr<std::vector<transaction::pending_tx>> &tv)
            : id(_id)
            , client(cl)
            , count(0)
            , timestamp(vclk)
            , tx_vec(tv)
        { }
    };
}

#endif
