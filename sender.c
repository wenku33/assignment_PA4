#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "multicast.h"
#include "packet.h"
#include <time.h>

typedef struct {
    int packets_sent;
    int bytes_sent;
    int cycles;
    struct timespec start;
} sender_stats_t;

unsigned int compute_checksum(char *data, int len) {
    unsigned int sum = 0;
    for (int i = 0; i < len; i++) {
        sum += (unsigned char)data[i]; // add up all byte values in a buffer
    }
    return sum;
}

void resend_chunk(mcast_t *m, const char *filepath, int file_id, int seq, int chunk_size) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("fopen resend");
        return;
    }

    // seeks to the requested chunk offset and rereads just that chunk
    long offset = (long)seq * chunk_size;
    if (fseek(file, offset, SEEK_SET) != 0) {
        perror("fseek resend");
        fclose(file);
        return;
    }

    char *buffer = malloc(chunk_size);
    if (!buffer) {
        perror("malloc resend");
        fclose(file);
        return;
    }

    int bytes_read = (int)fread(buffer, 1, chunk_size, file);
    if (bytes_read <= 0) {
        printf("Nothing to resend for file_id=%d seq=%d\n", file_id, seq);
        free(buffer);
        fclose(file);
        return;
    }

    // creates a new data packet with the same chunk data and metadata and resends it
    data_packet_t *data_pkt = malloc(sizeof(data_packet_t) + bytes_read);
    if (!data_pkt) {
        perror("malloc resend packet");
        free(buffer);
        fclose(file);
        return;
    }

    data_pkt->packet_type = 2;
    data_pkt->data_size = bytes_read;
    data_pkt->file_id = file_id;
    data_pkt->seq_num = seq;
    data_pkt->checksum = compute_checksum(buffer, bytes_read);
    memcpy(data_pkt->data, buffer, bytes_read);

    multicast_send(m, data_pkt, sizeof(data_packet_t) + bytes_read);
    printf("Resent chunk %d for file_id=%d (%d bytes)\n", seq, file_id, bytes_read);

    free(data_pkt);
    free(buffer);
    fclose(file);
}

void process_retrans_requests(mcast_t *m, const char *filepath, const char *filename, int file_id, int chunk_size, int rounds) {
    for (int r = 0; r < rounds; r++) {
        int rc = multicast_check_receive(m);
        if (rc <= 0) {
            continue;
        }

        char buf[MAX_PACKET_SIZE];
        int n = multicast_receive(m, buf, sizeof(buf));
        if (n <= 0) {
            continue;
        }

        int type = *(int *)buf;

        if (type == 3) {
            retrans_packet_t *req = (retrans_packet_t *)buf; // resends the requested chunk
            if (req->file_id != file_id) {
                printf("Ignoring retrans request for file_id=%d while handling %d\n", req->file_id, file_id);
                continue;
            }
            if (req->seq_num < 0) {
                printf("Ignoring invalid retrans request seq=%d\n", req->seq_num);
                continue;
            }
            printf("Received retransmission request: file='%s' seq=%d\n", filename, req->seq_num);
            resend_chunk(m, filepath, file_id, req->seq_num, chunk_size);

        } else if (type == 5) { // acknowledgment that the receiver has received all retransmissions for this file and is done with it
            retrans_recvd_packet_t *ack = (retrans_recvd_packet_t *)buf; // retransmission complete
            printf("Received retransmission-complete packet for file_id=%d\n", ack->file_id);
            return; // stop processing retrans requests for this file (exit early)
        } else {
            printf("Received non-retrans packet type %d during retrans window\n", type);
        }
    }
}

int main(int argc, char *argv[]) {
    mcast_t *m = multicast_init("239.255.0.1", 5001, 5002);
    multicast_setup_recv(m);
    int chunk_size = 1024;
    char *files[100];
    int file_count = 0;

    setbuf(stdout, NULL);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                printf("Error\n");
                return 1;
            }

            chunk_size = atoi(argv[++i]);
            if (chunk_size <= 0 ||
                chunk_size > (MAX_PACKET_SIZE - (int)sizeof(data_packet_t))) {
                printf("Error: invalid chunk size. Max allowed is %d\n", MAX_PACKET_SIZE - (int)sizeof(data_packet_t));
                return 1;
            }
        } else {
            files[file_count++] = argv[i];
        }
    }

    if (file_count == 0) {
        printf("Invalid Input");
        return 1;
    }

    sender_stats_t stats = {0};

    printf("Chunk size: %d bytes, sending %d file(s)\n", chunk_size, file_count);

    int cycle = 0;
    while (1) {
        printf("\n Cycle %d \n", cycle++);

        int prev_packets = stats.packets_sent;
        struct timespec cycle_start, cycle_end;
        clock_gettime(CLOCK_MONOTONIC, &cycle_start);

        // calculate file size and total chunks
        for (int i = 0; i < file_count; i++) {
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "./share/%s", files[i]);

            FILE *file = fopen(filepath, "rb");
            if (!file) {
                perror("fopen");
                continue;
            }

            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            rewind(file);

            int total_chunks = (int)((file_size + chunk_size - 1) / chunk_size);
            printf("File %d: %s (%ld bytes)\n", i, files[i], file_size);

            // file definition packet 
            file_defn_packet_t def_pkt;
            memset(&def_pkt, 0, sizeof(def_pkt));  // zero first
            def_pkt.packet_type = 1;
            def_pkt.file_id = i;
            strncpy(def_pkt.file_name, files[i], sizeof(def_pkt.file_name) - 1);
            def_pkt.total_chunks = total_chunks;
            def_pkt.chunk_size = chunk_size;

            // checksum computation last, after memset
            unsigned int fchecksum = 0;
            unsigned char cbuf[4096];
            size_t n;
            while ((n = fread(cbuf, 1, sizeof(cbuf), file)) > 0) {
                for (size_t k = 0; k < n; k++) fchecksum += cbuf[k];
            }
            rewind(file);  // reset for chunk sending
            def_pkt.file_checksum = fchecksum;  

            // send file definition packet
            multicast_send(m, &def_pkt, sizeof(def_pkt));
            printf("Sent definition for '%s': %d chunks\n", def_pkt.file_name, total_chunks);
            usleep(100000);

            char *buffer = malloc(chunk_size);
            if (!buffer) {
                perror("malloc");
                fclose(file);
                continue;
            }

            // send data chunks
            int seq = 0;
            int bytes_read;
            while ((bytes_read = (int)fread(buffer, 1, chunk_size, file)) > 0) {
                data_packet_t *data_pkt = malloc(sizeof(data_packet_t) + bytes_read);
                if (!data_pkt) {
                    perror("malloc");
                    free(buffer);
                    fclose(file);
                    return 1;
                }

                data_pkt->packet_type = 2;
                data_pkt->data_size = bytes_read;
                data_pkt->file_id = i;
                data_pkt->seq_num = seq; // tells the receiver where the chunk belongs in the file
                data_pkt->checksum = compute_checksum(buffer, bytes_read);
                memcpy(data_pkt->data, buffer, bytes_read);

                multicast_send(m, data_pkt, sizeof(data_packet_t) + bytes_read);
                stats.packets_sent++;
                stats.bytes_sent += sizeof(data_packet_t) + bytes_read;
                printf("Sent chunk %d/%d for '%s' (%d bytes)\n",
                    seq + 1, total_chunks, files[i], bytes_read);

                free(data_pkt);
                seq++;
                usleep(20000); // reduce packet flooding and make reception more stable
            }

            free(buffer);

            // send end-of-file packet
            end_packet_t end_pkt;
            end_pkt.packet_type = 4;
            end_pkt.file_id = i;
            multicast_send(m, &end_pkt, sizeof(end_pkt));
            printf("Sent end packet for '%s'\n", files[i]);

            // waits for a limited number of polling rounds and listens for incoming packets (retransmission window)
            process_retrans_requests(m, filepath, files[i], i, chunk_size, 10); // 10 arbitrary rounds for checking retrans requests -> change?

            fclose(file);
            usleep(100000);
        }

        // stats 
        stats.cycles++;
        clock_gettime(CLOCK_MONOTONIC, &cycle_end);
        double elapsed = (cycle_end.tv_sec - stats.start.tv_sec) + (cycle_end.tv_nsec - stats.start.tv_nsec) / 1e9;
        double cycle_time = (cycle_end.tv_sec - cycle_start.tv_sec) + (cycle_end.tv_nsec - cycle_start.tv_nsec) / 1e9;
        int cycle_packets = stats.packets_sent - prev_packets; // packets sent in this cycle

        printf("\nSENDER STATS: \n");
        printf("  Cycles:           %d\n", stats.cycles);
        printf("  Packets sent:     %d\n", stats.packets_sent);
        printf("  Bytes sent:       %d\n", stats.bytes_sent);
        printf("  Throughput:       %.9f packets/sec\n", stats.packets_sent / elapsed);
        printf("  Session time:     %.2f sec\n", elapsed);

        sleep(2); 
    }
}