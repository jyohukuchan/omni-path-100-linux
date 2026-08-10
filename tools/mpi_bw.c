// SPDX-License-Identifier: GPL-2.0-only
/* Unidirectional streaming bandwidth across two hosts.
 *
 * Pairing is derived from the actual processor names, so a rank is never
 * paired with a rank on its own host; that would measure shared memory
 * instead of the fabric.  Rank 0 prints the pairing it used, so the mapping
 * can be checked against the intent of the run.
 *
 * usage: mpi_bw [bytes] [window] [rounds] [mode]
 *   mode=0  the host holding rank 0 sends
 *   mode=1  the other host sends
 *   mode=2  both directions at once; the reported aggregate counts both, so on
 *           a full-duplex link it can exceed the one-way line rate
 *
 * Build: mpicc -O2 -o mpi_bw mpi_bw.c
 * Run:   mpirun --hostfile hf -np 8 --map-by ppr:4:node \
 *               --mca pml cm --mca mtl psm2 --bind-to none \
 *               hfi_local_run mpi_bw 8388608 8 100 0
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAMELEN MPI_MAX_PROCESSOR_NAME

int main(int argc, char **argv)
{
	int rank, size, i, w, peer = -1, sender = 0, pairs = 0, flip = 0, bidir = 0;
	size_t bytes = 8u << 20;
	int window = 8, rounds = 100;
	char name[NAMELEN], *all;
	char host0[NAMELEN] = "", host1[NAMELEN] = "";
	int *lista, *listb, na = 0, nb = 0, len;
	double t0, t1, secs, maxsecs = 0.0, minsecs = 0.0;
	char *buf, *rbuf = NULL;
	MPI_Request *req;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	if (argc > 1) bytes  = strtoull(argv[1], NULL, 0);
	if (argc > 2) window = atoi(argv[2]);
	if (argc > 3) rounds = atoi(argv[3]);
	if (argc > 4) { int m = atoi(argv[4]); bidir = (m == 2); flip = (m == 1); }

	MPI_Get_processor_name(name, &len);
	name[len] = '\0';
	all = malloc((size_t)size * NAMELEN);
	MPI_Allgather(name, NAMELEN, MPI_CHAR, all, NAMELEN, MPI_CHAR, MPI_COMM_WORLD);

	lista = malloc(sizeof(int) * size);
	listb = malloc(sizeof(int) * size);
	for (i = 0; i < size; i++) {
		char *n = all + (size_t)i * NAMELEN;
		if (!host0[0]) strcpy(host0, n);
		if (!strcmp(n, host0)) { lista[na++] = i; continue; }
		if (!host1[0]) strcpy(host1, n);
		if (!strcmp(n, host1)) { listb[nb++] = i; continue; }
		if (!rank) fprintf(stderr, "more than two hosts\n");
		MPI_Abort(MPI_COMM_WORLD, 1);
	}
	if (na != nb || na == 0) {
		if (!rank) fprintf(stderr, "need the same number of ranks on each of two hosts (%d vs %d)\n", na, nb);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}
	pairs = na;
	for (i = 0; i < na; i++) {
		if (lista[i] == rank) { peer = listb[i]; sender = !flip; }
		if (listb[i] == rank) { peer = lista[i]; sender = flip; }
	}
	if (bidir) sender = 1;   /* every rank both sends and receives */
	if (!rank) {
		if (bidir)
			printf("pairing: %s <-> %s (%d pairs, bidirectional):", host0, host1, na);
		else
			printf("pairing: %s -> %s (%d pairs):", flip ? host1 : host0, flip ? host0 : host1, na);
		for (i = 0; i < na; i++) printf(" %d->%d", lista[i], listb[i]);
		printf("\n");
		fflush(stdout);
	}

	buf = malloc(bytes * (size_t)window);
	req = malloc(sizeof(*req) * (size_t)window * (bidir ? 2 : 1));
	if (bidir) {
		rbuf = malloc(bytes * (size_t)window);
		if (!rbuf) MPI_Abort(MPI_COMM_WORLD, 1);
		memset(rbuf, 0, bytes * (size_t)window);
	}
	if (!buf || !req) MPI_Abort(MPI_COMM_WORLD, 1);
	memset(buf, rank + 1, bytes * (size_t)window);

	for (i = 0; i < 2; i++) {          /* warm-up */
		for (w = 0; w < window; w++) {
			if (bidir) {
				MPI_Irecv(rbuf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[window + w]);
				MPI_Isend(buf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[w]);
			} else if (sender) {
				MPI_Isend(buf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[w]);
			} else {
				MPI_Irecv(buf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[w]);
			}
		}
		MPI_Waitall(window * (bidir ? 2 : 1), req, MPI_STATUSES_IGNORE);
	}
	MPI_Barrier(MPI_COMM_WORLD);

	t0 = MPI_Wtime();
	for (i = 0; i < rounds; i++) {
		for (w = 0; w < window; w++) {
			if (bidir) {
				MPI_Irecv(rbuf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[window + w]);
				MPI_Isend(buf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[w]);
			} else if (sender) {
				MPI_Isend(buf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[w]);
			} else {
				MPI_Irecv(buf + (size_t)w * bytes, (int)bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, &req[w]);
			}
		}
		MPI_Waitall(window * (bidir ? 2 : 1), req, MPI_STATUSES_IGNORE);
	}
	t1 = MPI_Wtime();
	secs = t1 - t0;
	/* Senders only: the aggregate is limited by the slowest pair, so report
	 * the fastest and slowest sender as well to expose a single straggler. */
	if (!sender)
		secs = 0.0;
	MPI_Reduce(&secs, &maxsecs, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
	{
		double inv = sender ? secs : 1e30, mininv;
		MPI_Reduce(&inv, &mininv, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
		if (!rank) minsecs = mininv;
	}

	if (!rank) {
		double total = (double)bytes * window * rounds * pairs * (bidir ? 2 : 1);
		double per_pair = (double)bytes * window * rounds * (bidir ? 2 : 1);
		printf("pairs=%d msg_bytes=%zu window=%d rounds=%d total_GiB=%.2f seconds=%.3f aggregate=%.2f Gb/s"
		       " fastest_pair=%.2f Gb/s slowest_pair=%.2f Gb/s\n",
		       pairs, bytes, window, rounds, total / (1024.0 * 1024.0 * 1024.0), maxsecs,
		       total * 8.0 / maxsecs / 1e9,
		       per_pair * 8.0 / minsecs / 1e9,
		       per_pair * 8.0 / maxsecs / 1e9);
	}
	free(buf); free(rbuf); free(req); free(all); free(lista); free(listb);
	MPI_Finalize();
	return 0;
}
