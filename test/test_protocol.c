/* Simple simulator for protocol. */
#include "config.h"
#include <assert.h>
#include <ccan/err/err.h>
#include <ccan/read_write_all/read_write_all.h>
#include <ccan/short_types/short_types.h>
#include <ccan/str/str.h>
#include <ccan/structeq/structeq.h>
#include <ccan/tal/tal.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct funding {
	/* inhtlcs = htlcs they offered, outhtlcs = htlcs we offered */
	u32 inhtlcs, outhtlcs;
};

/* We keep one for them, one for us. */
struct commit_info {
	struct commit_info *prev;
	/* How deep we are */
	unsigned int number;
	/* Channel state. */
	struct funding funding;
	/* Pending changes (for next commit_info) */
	int *changes_incoming, *changes_outgoing;
	/* Have sent/received revocation secret. */
	bool revoked;
	/* Have their signature, ie. can be broadcast */
	bool counterparty_signed;
};

/* A "signature" is a copy of the commit tx state, for easy diagnosis. */
struct signature {
	struct funding f;
};

struct peer {
	int infd, outfd, cmdfd, cmddonefd;

	/* Last one is the one we're changing. */
	struct commit_info *us, *them;
};

static u32 htlc_mask(unsigned int htlc)
{
	if (htlc > 32)
		errx(1, "HTLC number %u too large", htlc);
	if (!htlc)
		errx(1, "HTLC number can't be zero");
	return (1U << (htlc-1));
}

static bool have_htlc(u32 htlcs, unsigned int htlc)
{
	return htlcs & htlc_mask(htlc);
}

static bool add_change_internal(struct commit_info *ci, int **changes,
				const u32 *add, const u32 *remove,
				int c)
{
	size_t i, n = tal_count(*changes);
	
	if (!c)
		errx(1, "INTERNAL: Adding zero change?");

	/* Can't add/remove it already there/absent. */
	if (c > 0 && have_htlc(*add, c))
		return false;
	else if (c < 0 && !have_htlc(*remove, -c))
		return false;

	/* Can't request add/remove twice. */
	for (i = 0; i < n; i++)
		if ((*changes)[i] == c)
			return false;

	tal_resize(changes, n+1);
	(*changes)[n] = c;
	return true;
}

/*
 * Once we're sending/receiving revoke, we queue the changes on the
 * alternate side.
 */
static bool queue_changes_other(struct commit_info *ci, int *changes)
{
	size_t i, n = tal_count(changes);

	for (i = 0; i < n; i++) {
		if (!add_change_internal(ci, &ci->changes_outgoing,
					 &ci->funding.outhtlcs,
					 &ci->funding.inhtlcs,
					 changes[i]))
			return false;
	}
	return true;
}

/*
 * Normally, we add incoming changes.
 */
static bool add_incoming_change(struct commit_info *ci, int c)
{
	return add_change_internal(ci, &ci->changes_incoming,
				   &ci->funding.inhtlcs, &ci->funding.outhtlcs,
				   c);
}

static struct commit_info *new_commit_info(const tal_t *ctx,
					   struct commit_info *prev)
{
	struct commit_info *ci = talz(ctx, struct commit_info);
	ci->prev = prev;
	ci->changes_incoming = tal_arr(ci, int, 0);
	ci->changes_outgoing = tal_arr(ci, int, 0);
	if (prev) {
		ci->number = prev->number + 1;
		ci->funding = prev->funding;
	}
	return ci;
}

/* Each side can add their own incoming HTLC, or close your outgoing HTLC. */
static void do_change(u32 *add, u32 *remove, int htlc)
{
	if (htlc < 0) {
		if (!have_htlc(*remove, -htlc))
			errx(1, "Already removed htlc %u", -htlc);
		*remove &= ~htlc_mask(-htlc);
	} else {
		if (htlc == 0)
			errx(1, "zero change?");
		if (have_htlc(*add, htlc))
			errx(1, "Already added htlc %u", htlc);
		*add |= htlc_mask(htlc);
	}
}

/* We duplicate the commit info, with the changes applied. */
static struct commit_info *apply_changes(const tal_t *ctx,
					 struct commit_info *old)
{
	struct commit_info *ci = new_commit_info(ctx, old);
	size_t i, n;

	/* Changes they offered. */
	n = tal_count(old->changes_incoming);
	for (i = 0; i < n; i++)
		do_change(&ci->funding.inhtlcs,
			  &ci->funding.outhtlcs,
			  old->changes_incoming[i]);

	/* Changes we offered. */
	n = tal_count(old->changes_outgoing);
	for (i = 0; i < n; i++)
		do_change(&ci->funding.outhtlcs,
			  &ci->funding.inhtlcs,
			  old->changes_outgoing[i]);
 
	return ci;
}

static struct signature commit_sig(const struct commit_info *ci)
{
	struct signature sig;
	sig.f = ci->funding;
	return sig;
}

static void write_out(int fd, const void *p, size_t len)
{
	if (!write_all(fd, p, len))
		err(1, "Writing to peer");
}

static void dump_htlcs(u32 htlcs)
{
	unsigned int i;

	for (i = 1; i <= 32; i++)
		if (have_htlc(htlcs, i))
			printf(" %u", i);
}

static void dump_commit_info(const struct commit_info *ci)
{
	size_t i, n;

	printf(" Commit %u:", ci->number);
	printf("\n  Offered htlcs:");
	dump_htlcs(ci->funding.outhtlcs);
	printf("\n  Received htlcs:");
	dump_htlcs(ci->funding.inhtlcs);

	n = tal_count(ci->changes_incoming);
	if (n > 0) {
		printf("\n  Pending incoming:");
		for (i = 0; i < n; i++)
			printf(" %+i", ci->changes_incoming[i]);
	}

	n = tal_count(ci->changes_outgoing);
	if (n > 0) {
		printf("\n  Pending outgoing:");
		for (i = 0; i < n; i++)
			printf(" %+i", ci->changes_outgoing[i]);
	}

	if (ci->counterparty_signed)
		printf("\n  SIGNED");
	if (ci->revoked)
		printf("\n  REVOKED");
	printf("\n");
	fflush(stdout);
}

static void dump_rev(const struct commit_info *ci, bool all)
{
	if (ci->prev)
		dump_rev(ci->prev, all);
	if (all || !ci->revoked)
		dump_commit_info(ci);
}

static void dump_peer(const struct peer *peer, bool all)
{
	printf("OUR COMMITS:\n");
	dump_rev(peer->us, all);

	printf("THEIR COMMITS:\n");
	dump_rev(peer->them, all);
}

static void read_in(int fd, void *p, size_t len)
{
	alarm(5);
	if (!read_all(fd, p, len))
		err(1, "Reading from peer");
	alarm(0);
}

static void read_peer(struct peer *peer, const char *str, const char *cmd)
{
	char *p = tal_arr(peer, char, strlen(str)+1);
	read_in(peer->infd, p, strlen(str));
	p[strlen(str)] = '\0';
	if (!streq(p, str))
		errx(1, "%s: Expected %s from peer, got %s", cmd, str, p);
	tal_free(p);
}

/* Offers HTLC:
 * - Record the change to them.
 */
static void send_offer(struct peer *peer, unsigned int htlc)
{
	/* Can't have sent already. */
	if (!add_incoming_change(peer->them, htlc))
		errx(1, "offer: already offered %u", htlc);
	write_out(peer->outfd, "+", 1);
	write_out(peer->outfd, &htlc, sizeof(htlc));
}

/* Removes HTLC:
 * - Record the change to them.
 */
static void send_remove(struct peer *peer, unsigned int htlc)
{
	/* Can't have removed already. */
	if (!add_incoming_change(peer->them, -htlc))
		errx(1, "remove: already removed of %u", htlc);
	write_out(peer->outfd, "-", 1);
	write_out(peer->outfd, &htlc, sizeof(htlc));
}

/* FIXME:
 * We don't enforce the rule that commits have to wait for revoke response
 * before the next one.  It might be simpler if we did?
 */
static struct commit_info *last_unrevoked(struct commit_info *ci)
{
	struct commit_info *next = NULL;

	/* If this is already revoked, all are. */
	if (ci->revoked)
		return NULL;
	
	/* Find revoked commit; one we hit before that was last unrevoked. */
	for (; ci; next = ci, ci = ci->prev) {
		if (ci->revoked)
			break;
	}
	return next;
}

/* Commit:
 * - Apply changes to them.
 */
static void send_commit(struct peer *peer)
{
	struct signature sig;

	/* Must have changes. */
	if (tal_count(peer->them->changes_incoming) == 0
	    && tal_count(peer->them->changes_outgoing) == 0)
		errx(1, "commit: no changes to commit");

	peer->them = apply_changes(peer, peer->them);
	sig = commit_sig(peer->them);
	peer->them->counterparty_signed = true;

	/* Tell other side about commit and result (it should agree!) */
	write_out(peer->outfd, "C", 1);
	write_out(peer->outfd, &sig, sizeof(sig));
}

/* Receive revoke:
 * - Queue pending changes to us.
 */
static void receive_revoke(struct peer *peer, u32 number)
{
	struct commit_info *ci = last_unrevoked(peer->them);

	if (!ci)
		errx(1, "receive_revoke: no commit to revoke");
	if (ci->number != number)
		errx(1, "receive_revoke: revoked %u but %u is next",
		     number, ci->number);
	ci->revoked = true;
	if (!ci->counterparty_signed)
		errx(1, "receive_revoke: revoked unsigned commit?");

	/* The changes we sent with that commit, add them to us. */
	if (!queue_changes_other(peer->us, ci->changes_incoming))
		errx(1, "receive_revoke: could not add their changes to us");

	/* Cleans up dump output now we've consumed them. */
	tal_free(ci->changes_incoming);
	ci->changes_incoming = tal_arr(ci, int, 0);
}

/* Receives HTLC offer:
 * - Record the change to us.
 */
static void receive_offer(struct peer *peer, unsigned int htlc)
{
	if (!add_incoming_change(peer->us, htlc))
		errx(1, "receive_offer: already offered of %u", htlc);
}

/* Receive HTLC remove:
 * - Record the change to us.
 */
static void receive_remove(struct peer *peer, unsigned int htlc)
{
	if (!add_incoming_change(peer->us, -htlc))
		errx(1, "receive_remove: already removed %u", htlc);
}

/* Send revoke.
 * - Queue changes to them.
 */
static void send_revoke(struct peer *peer, struct commit_info *ci)
{
	/* We always revoke in order. */
	assert(!ci->prev || ci->prev->revoked);
	assert(ci->counterparty_signed);
	assert(!ci->revoked);
	ci->revoked = true;

	/* Queue changes. */
	if (!queue_changes_other(peer->them, ci->changes_incoming))
		errx(1, "Failed queueing changes to them for send_revoke");

	/* Clean up for dump output. */
	tal_free(ci->changes_incoming);
	ci->changes_incoming = tal_arr(ci, int, 0);
	
	write_out(peer->outfd, "R", 1);
	write_out(peer->outfd, &ci->number, sizeof(ci->number));
}

/* Receive commit:
 * - Apply changes to us.
 */
static void receive_commit(struct peer *peer, const struct signature *sig)
{
	struct signature oursig;

	/* Must have changes. */
	if (tal_count(peer->us->changes_incoming) == 0
	    && tal_count(peer->us->changes_outgoing) == 0)
		errx(1, "receive_commit: no changes to commit");

	peer->us = apply_changes(peer, peer->us);
	oursig = commit_sig(peer->us);
	if (!structeq(sig, &oursig))
		errx(1, "Commit state %#x/%#x, they gave %#x/%#x",
		     sig->f.inhtlcs, sig->f.outhtlcs,
		     oursig.f.inhtlcs, oursig.f.outhtlcs);
	peer->us->counterparty_signed = true;
	send_revoke(peer, peer->us->prev);
}

static void do_cmd(struct peer *peer)
{
	char cmd[80];
	int i;
	unsigned int htlc;
	struct commit_info *ci;

	for (i = 0; i < sizeof(cmd); i++) {
		if (!read_all(peer->cmdfd, cmd+i, 1))
			err(1, "Reading command pipe");
		if (!cmd[i])
			break;
	}

	if (i == 0)
		exit(0);
	
	if (sscanf(cmd, "offer %u", &htlc) == 1)
		send_offer(peer, htlc);
	else if (sscanf(cmd, "remove %u", &htlc) == 1)
		send_remove(peer, htlc);
	else if (streq(cmd, "commit"))
		send_commit(peer);
	else if (streq(cmd, "recvrevoke")) {
		u32 number;
		read_peer(peer, "R", cmd);
		read_in(peer->infd, &number, sizeof(number));
		receive_revoke(peer, number);
	} else if (streq(cmd, "recvoffer")) {
		read_peer(peer, "+", cmd);
		read_in(peer->infd, &htlc, sizeof(htlc));
		receive_offer(peer, htlc);
	} else if (streq(cmd, "recvremove")) {
		read_peer(peer, "-", cmd);
		read_in(peer->infd, &htlc, sizeof(htlc));
		receive_remove(peer, htlc);
	} else if (streq(cmd, "recvcommit")) {
		struct signature sig;
		read_peer(peer, "C", cmd);
		read_in(peer->infd, &sig, sizeof(sig));
		receive_commit(peer, &sig);
	} else if (streq(cmd, "dump")) {
		dump_peer(peer, false);
	} else if (streq(cmd, "dumpall")) {
		dump_peer(peer, true);
	} else
		errx(1, "Unknown command %s", cmd);

	/* We must always have (at least one) signed, unrevoked commit. */
	for (ci = peer->us; ci; ci = ci->prev) {
		if (ci->counterparty_signed && !ci->revoked) {
			write(peer->cmddonefd, "", 1);
			return;
		}
	}
	errx(1, "No signed, unrevoked commit!");
}

static void new_peer(int infdpair[2], int outfdpair[2], int cmdfdpair[2],
		     int cmddonefdpair[2])
{
	struct peer *peer;

	switch (fork()) {
	case 0:
		break;
	case -1:
		err(1, "Forking");
	default:
		return;
	}

	close(infdpair[1]);
	close(outfdpair[0]);
	close(cmdfdpair[1]);
	close(cmddonefdpair[0]);

	peer = tal(NULL, struct peer);
	/* Create first, signed commit info. */
	peer->us = new_commit_info(peer, NULL);
	peer->us->counterparty_signed = true;
	
	peer->them = new_commit_info(peer, NULL);
	peer->them->counterparty_signed = true;

	peer->infd = infdpair[0];
	peer->outfd = outfdpair[1];
	peer->cmdfd = cmdfdpair[0];
	peer->cmddonefd = cmddonefdpair[1];

	while (1)
		do_cmd(peer);
}

int main(int argc, char *argv[])
{
	char cmd[80];
	int a_to_b[2], b_to_a[2], acmd[2], bcmd[2], adonefd[2], bdonefd[2];

	err_set_progname(argv[0]);

	if (pipe(a_to_b) || pipe(b_to_a) || pipe(adonefd) || pipe(acmd))
		err(1, "Creating pipes");

	new_peer(a_to_b, b_to_a, acmd, adonefd);

	if (pipe(bdonefd) || pipe(bcmd))
		err(1, "Creating pipes");

	new_peer(b_to_a, a_to_b, bcmd, bdonefd);

	close(acmd[0]);
	close(bcmd[0]);
	close(adonefd[1]);
	close(bdonefd[1]);
	close(b_to_a[0]);
	close(b_to_a[1]);
	close(a_to_b[0]);
	close(a_to_b[1]);

	while (fgets(cmd, sizeof(cmd), stdin)) {
		int cmdfd, donefd;

		if (!strends(cmd, "\n"))
			errx(1, "Truncated command");
		cmd[strlen(cmd)-1] = '\0';
		
		if (strstarts(cmd, "A:")) {
			cmdfd = acmd[1];
			donefd = adonefd[0];
		} else if (strstarts(cmd, "B:")) { 
			cmdfd = bcmd[1];
			donefd = bdonefd[0];
		} else if (strstarts(cmd, "echo ")) {
			printf("%s\n", cmd + 5);
			fflush(stdout);
			continue;
		} else if (strstarts(cmd, "#") || streq(cmd, ""))
			continue;
		else
			errx(1, "Unknown command %s", cmd);

		if (!write_all(cmdfd, cmd+2, strlen(cmd)-1))
			errx(1, "Sending %s", cmd);
		alarm(5);
		if (!read_all(donefd, &cmdfd, 1))
			errx(1, "Failed on cmd %s", cmd);
		alarm(0);
	}
	write_all(acmd[1], "", 1);
	write_all(bcmd[1], "", 1);
	return 0;
}
