all:
	rm -f all_reduce_ring
	gcc all_reduce_ring.c rdma_utils.c -libverbs -o all_reduce_ring -lm
tar:
	tar -cvzf 303062087_207129420.tgz Makefile all_reduce_ring.c all_reduce_ring.h rdma_utils.c rdma_utils.h