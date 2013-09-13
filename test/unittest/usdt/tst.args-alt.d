/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2013 Oracle, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/* @@trigger: usdt-tst-args */
/* @@trigger-timing: before */
/* @@runtest-opts: $_pid */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * Ensure that arguments to USDT probes can be retrieved both from registers
 * and the stack.
 */
BEGIN
{
	/* Timeout after 5 seconds */
	timeout = timestamp + 5000000000;
}

test_prov$1:::place
/arg0 == 10 && arg1 ==  4 && arg2 == 20 && arg3 == 30 && arg4 == 40 &&
 arg5 == 50 && arg6 == 60 && arg7 == 70 && (int)arg8 == 80 && arg9 == 90/
{
	exit(0);
}

test_prov$1:::place
{
	printf("args are %d, %d, %d, %d, %d, %d, %d, %d, %d, %d\n",
	       arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, (int)arg8, arg9);
	exit(1);
}

profile:::tick-1
/timestamp > timeout/
{
	trace("test timed out");
	exit(1);
}