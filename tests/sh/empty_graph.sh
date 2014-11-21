#! /bin/bash
#
# empty_graph.sh
# Copyright (C) 2014 Ayush Dubey <dubey@cs.cornell.edu>
#
# See the LICENSE file for licensing agreement
#

tests/sh/setup.sh
python tests/python/correctness/empty_graph_sanity_checks.py
status=$?
tests/sh/clean.sh

exit $status
