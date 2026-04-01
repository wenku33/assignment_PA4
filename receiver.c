#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "multicast.h"
#include "packet.h"

unsigned int compute_checksum(char *data, int len) {
    unsigned int sum = 0;

    for (int i = 0; i < len; i++) {
        sum += (unsigned char)data[i];
    }

    return sum;
}

int verify_checksum(data_packet_t *pkt) {
    unsigned int computed = compute_checksum(pkt->data, pkt->data_size);

    return (computed == pkt->checksum);
}

int main(int argc, char *argv[]) {
    printf("Receiver is listening...\n");
    mcast_t *m = multicast_init("233.3.3.3", 5002, 5001);
    multicast_setup_recv(m);
    // Receiver setup 
    file_defn_packet_t pkt;
    char buffer[MAX_PACKET_SIZE];
    // Is there another way to dynamically set buffer's size?

    file_state_t state[MAX_FILES];
    for (int i = 0; i < MAX_FILES; i++) {
        memset(&state[i], 0, sizeof(state));
    }

    while(1) {
        // int check = multicast_check_receive(m);
        // printf("check is %d\n", check);
        if (multicast_check_receive(m) > 0) { 
            
            int n = multicast_receive(m, buffer, sizeof(buffer));
            printf("\nReceived packet of size %d\n", n);

            int type = *(int *)buffer;
            printf("packet type %d\n", type);


            if (type == 1) { // Receive file definition Packet
                file_defn_packet_t *def_pkt = (file_defn_packet_t *)buffer;
                // if (n != sizeof(file_defn_packet_t)) continue; // Maybe don't do this

                // Initialize file state
                int file_id = def_pkt->file_id;
                if (state[file_id].chunks == NULL) { 
                    state[file_id].total_chunks = def_pkt->total_chunks;
                    strcpy(state[file_id].file_name, def_pkt->file_name); 
                    state[file_id].general_chunk_size = def_pkt-> chunk_size;

                    state[file_id].chunks = malloc(def_pkt->total_chunks * sizeof(char *));
                    state[file_id].chunk_sizes = malloc(def_pkt->total_chunks * sizeof(int));

                    for (int i = 0; i < def_pkt->total_chunks; i++) {
                        state[file_id].chunks[i] = NULL;
                    }
                    printf("Initialized state for file %s\n\n", state[file_id].file_name);
                
                }

            } else if (type == 2) { // Receive data packet
                data_packet_t *d_pkt = (data_packet_t *)buffer; 
                // if (n < sizeof(data_packet_t)) continue; // Maybe don't do this

                // if (n != sizeof(data_packet_t) + d_pkt->data_size) continue; // Maybe don't do this

                if (compute_checksum(d_pkt->data, d_pkt->data_size) != d_pkt->checksum) {
                    printf("Corrupted packet\n");
                    // make sender resend?? Drop for now!
                    continue;
                }

                
                int file_id = d_pkt->file_id;
                int seq = d_pkt->seq_num;

                // if (state[file_id]) Not initialized, ask for defn packet

                if (state[file_id].chunks[seq] == NULL) {
                    state[file_id].chunks[seq] = malloc(d_pkt->data_size);
                    memcpy(state[file_id].chunks[seq], d_pkt->data, d_pkt->data_size);

                    state[file_id].chunk_sizes[seq] = d_pkt->data_size;
                    state[file_id].received_chunks++; 
                    printf("Copied into state chunk for seq nbr: %d\n", seq);
                }

                // check completion
                printf("Received chunks for file %d/%d\n", state[file_id].received_chunks, state[file_id].total_chunks);
                if (state[file_id].received_chunks == state[file_id].total_chunks) {
                    printf("File %d complete\n", file_id);
                    char path[100];
                    mkdir("./received_files", 0777);
                    sprintf(path, "./received_files/%s", state[file_id].file_name);
                    printf("path is %s\n", path);
                    FILE *out = fopen(path, "wb");
                    if (!out) {
                        perror("fopen");
                        return 0;
                    }

                    for (int i = 0; i < state[file_id].total_chunks; i++) {
                        if (state[file_id].chunks[i] == NULL) {
                            printf("Missing chunk %d\n", i);
                            fclose(out);
                            return 0;
                        }

                        fwrite(state[file_id].chunks[i], 1,
                            state[file_id].chunk_sizes[i], out);
                    }
                    fclose(out);
                    printf("File saved: %s\n", path);
                    // Cleanup code
                    // for (int i = 0; i < state[file_id].total_chunks; i++) {
                    //     free(state[file_id].chunks[i]);
                    // }
                    // free(state[file_id].chunks);
                    // free(state[file_id].chunk_sizes);
                    // multicast_destroy(m);
                    // return 0;

                }

            } else if (type == 4) { // End packet
                int retransmissioned = 0;
                end_packet_t *e_pkt = (end_packet_t *)buffer;
                int file_id = e_pkt->file_id;
                if (state[file_id].done) continue;
                for (int i = 0; i < state[file_id].total_chunks; i++) {
                    if (state[file_id].chunks[i] == NULL) {
                        printf("Missing chunk %d, requesting\n", i);
                        request_retrans(m, file_id, i);
                        retransmissioned = 1;
                        usleep(1000);
                    }
                }

                if (!retransmissioned) {
                    state[file_id].done = 1;
                } else {
                    // Send end packet
                    retrans_recvd_packet_t retrans_pkt;
                    retrans_pkt.packet_type = 5;
                    retrans_pkt.file_id = file_id;
                    multicast_send(m, &retrans_pkt, sizeof(retrans_recvd_packet_t));
                }
                
            }
        }
    }
    
}