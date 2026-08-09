// SPDX-License-Identifier: GPL-2.0-only
/* Two-host ping-pong latency.
 *
 * Reports half round-trip time per message size.  Per-iteration samples are
 * kept so the distribution is visible: a median plus a 99th percentile says
 * much more about an interconnect than a mean does.
 *
 * As in mpi_bw.c the pair is derived from the processor names, so the two
 * ranks are never co-located on one host.
 *
 * usage: mpi_lat [iters] [size ...]
 *   with no sizes, a default sweep from 8 B to 4 MiB is used
 *
 * Build: mpicc -O2 -o mpi_lat mpi_lat.c
 * Run:   mpirun --hostfile hf -np 2 --map-by ppr:1:node \
 *               --mca pml cm --mca mtl psm2 --bind-to none \
 *               hfi_local_run mpi_lat 10000
 */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAMELEN MPI_MAX_PROCESSOR_NAME

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	static const size_t defaults[] = {
		8, 64, 512, 4096, 16384, 32768, 63000, 65536,
		131072, 262144, 1048576, 4194304
	};
	int rank, size, i, s, len, iters = 10000, warmup = 200;
	int initiator = 0, peer = -1, nsizes;
	const size_t *sizes;
	size_t *parsed = NULL, maxsize = 0;
	char name[NAMELEN], *all;
	char host0[NAMELEN] = "", host1[NAMELEN] = "";
	char *buf;
	double *sample, t0;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	if (size != 2) {
		if (!rank) fprintf(stderr, "need exactly 2 ranks, one per host\n");
		MPI_Finalize();
		return 1;
	}
	if (argc > 1) iters = atoi(argv[1]);
	if (argc > 2) {
		nsizes = argc - 2;
		parsed = malloc(sizeof(*parsed) * nsizes);
		for (i = 0; i < nsizes; i++) parsed[i] = strtoull(argv[i + 2], NULL, 0);
		sizes = parsed;
	} else {
		nsizes = (int)(sizeof(defaults) / sizeof(defaults[0]));
		sizes = defaults;
	}
	for (i = 0; i < nsizes; i++) if (sizes[i] > maxsize) maxsize = sizes[i];

	MPI_Get_processor_name(name, &len);
	name[len] = '\0';
	all = malloc((size_t)size * NAMELEN);
	MPI_Allgather(name, NAMELEN, MPI_CHAR, all, NAMELEN, MPI_CHAR, MPI_COMM_WORLD);
	strcpy(host0, all);
	strcpy(host1, all + NAMELEN);
	if (!strcmp(host0, host1)) {
		if (!rank) fprintf(stderr, "both ranks are on %s; this would measure shared memory\n", host0);
		MPI_Abort(MPI_COMM_WORLD, 1);
	}
	initiator = (rank == 0);
	peer = 1 - rank;

	buf = malloc(maxsize ? maxsize : 1);
	sample = malloc(sizeof(*sample) * iters);
	if (!buf || !sample) MPI_Abort(MPI_COMM_WORLD, 1);
	memset(buf, rank + 1, maxsize ? maxsize : 1);

	if (!rank) {
		printf("ping-pong %s <-> %s, %d iterations per size\n", host0, host1, iters);
		printf("%10s %10s %10s %10s %10s\n", "bytes", "min_us", "median_us", "p99_us", "mean_us");
		fflush(stdout);
	}

	for (s = 0; s < nsizes; s++) {
		size_t n = sizes[s];

		for (i = 0; i < warmup; i++) {
			if (initiator) {
				MPI_Send(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD);
				MPI_Recv(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			} else {
				MPI_Recv(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				MPI_Send(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD);
			}
		}
		MPI_Barrier(MPI_COMM_WORLD);

		for (i = 0; i < iters; i++) {
			t0 = MPI_Wtime();
			if (initiator) {
				MPI_Send(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD);
				MPI_Recv(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			} else {
				MPI_Recv(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				MPI_Send(buf, (int)n, MPI_BYTE, peer, 0, MPI_COMM_WORLD);
			}
			sample[i] = (MPI_Wtime() - t0) * 1e6 / 2.0;   /* half round trip, us */
		}
		MPI_Barrier(MPI_COMM_WORLD);

		if (initiator) {
			double sum = 0.0;
			for (i = 0; i < iters; i++) sum += sample[i];
			qsort(sample, iters, sizeof(*sample), cmp_double);
			printf("%10zu %10.2f %10.2f %10.2f %10.2f\n", n,
			       sample[0], sample[iters / 2], sample[(int)(iters * 0.99)],
			       sum / iters);
			fflush(stdout);
		}
	}

	free(buf); free(sample); free(all); free(parsed);
	MPI_Finalize();
	return 0;
}
