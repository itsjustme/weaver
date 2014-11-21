#! /bin/bash
#
# transactions.sh
# Copyright (C) 2014 Ayush Dubey <dubey@cs.cornell.edu>
#
# See the LICENSE file for licensing agreement
#

tests/sh/setup.sh
python tests/python/correctness/transactions_ring.py
status=$?
tests/sh/clean.sh

exit $status
