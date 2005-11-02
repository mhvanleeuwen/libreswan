#!/bin/sh

TEST_PURPOSE=regress
TEST_PROB_REPORT=0
TEST_TYPE=ctltest

TESTNAME=ipsec.conf-alsoflip-01
TESTHOST=east

REF_CONSOLE_OUTPUT=east-console.txt

REF_CONSOLE_FIXUPS="nocr.sed script-only.sed ipsec-setup-sanitize.sed pluto-log-sanitize.sed"

INIT_SCRIPT=alsoflip_test.sh
