#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multicast.h"
#include "packet.h"

unsigned int compute_checksum(char *data, int len) {
    unsigned int sum = 0;

    for (int i = 0; i < len; i++) {
        sum += (unsigned char)data[i];
    }

    return sum;
}

void resend_chunk(mcast_t *m,
                           char *filename,
                           int file_id,
                           int seq,
                           int chunk_size) {

    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("fopen resend");
        return;
    }

    // Move to correct offset
    long offset = (long)seq * chunk_size;
    if (fseek(file, offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(file);
        return;
    }

    // Read chunk
    char *buffer = malloc(chunk_size);
    int bytes_read = fread(buffer, 1, chunk_size, file);

    if (bytes_read <= 0) {
        printf("Nothing to resend for seq %d\n", seq);
        free(buffer);
        fclose(file);
        return;
    }

    // Create packet
    data_packet_t *pkt =
        malloc(sizeof(data_packet_t) + bytes_read);

    pkt->packet_type = 2;
    pkt->file_id = file_id;
    pkt->seq_num = seq;
    pkt->data_size = bytes_read;
    pkt->checksum = compute_checksum(buffer, bytes_read);

    memcpy(pkt->data, buffer, bytes_read);

    // Send
    multicast_send(m, pkt,
        sizeof(data_packet_t) + bytes_read);

    printf("Resent chunk %d (%d bytes)\n", seq, bytes_read);

    free(pkt);
    free(buffer);
    fclose(file);
}

void retrans_recv(mcast_t *m, int is_awaiting_retrans, int file_id, int chunk_size) {
    int retransmission_complete = 0;
    while (!retransmission_complete) {
        if (multicast_check_receive(m) > 0) {
            char buf[MAX_PACKET_SIZE];
            int n = multicast_receive(m, buf, sizeof(buf));

            int type = *(int *)buf;

            if (type == 3) { // retrans request
                retrans_packet_t *req = (retrans_packet_t *)buf;

                resend_chunk(m, req->file_id, req->seq_num, req->filename, chunk_size);
                if (!is_awaiting_retrans) { // if is_awaiting_retrans == 0, Activates momentary polling
                    retransmission_complete = 1;
                }
            } else if (type == 5) {
                retrans_recvd_packet_t *recvd_pkt = (retrans_recvd_packet_t *)buf;
                int revd_file_id = recvd_pkt->file_id;
                if (file_id == revd_file_id) {
                    retransmission_complete = 1;
                }
                
                // NB Receiver may not have received all retransitted chunks.
                // Sender will just have to listen to more retransmission requests. 
            } else if (type == 6) {
                // Retransmit defn_packet
                if (!is_awaiting_retrans) { // if is_awaiting_retrans == 0, Activates momentary polling
                    retransmission_complete = 1;
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    mcast_t *m = multicast_init("233.3.3.3", 5001, 5002);
    int chunk_size = 512; // default
    char *files[100];     // store file names
    int file_count = 0;

    for (int i = 1; i < argc; i++) {
        printf("Argument %d: %s\n", i, argv[i]);
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -c requires a value\n");
                exit(1);
            }
            chunk_size = atoi(argv[++i]); // move to next arg
        } else {
            files[file_count++] = argv[i];
        }
    }

    if (file_count == 0) {
        printf("Usage: %s [-c chunk_size] <file1> <file2> ...\n", argv[0]);
        exit(1);
    }

    printf("Chunk size: %d\n", chunk_size);

    for (int i = 0; i < file_count; i++) {
        printf("File %d: %s\n", i, files[i]);
    }
    printf("filecount is %d\n",file_count);

    // Sending the files as packets
    for (int i = 0; i < file_count; i++) {
        char filename[100];
        sprintf(filename, "./share/%s", files[i]);
        printf("filename is %s\n",filename);
        FILE *file = fopen(filename, "rb");
        printf("file opened\n");
        if (!file) {
            perror("Error: fopen");
            exit(1);
        }

        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        rewind(file);
        printf("file rewinded\n");

        int total_chunks = (file_size + chunk_size - 1) / chunk_size; // Ceiling division
        printf("Total nbr of chunks: %d\n", total_chunks);
        

        // Send file definition packet 
        file_defn_packet_t def_pkt;
        def_pkt.packet_type = 1;
        def_pkt.file_id = i;
        def_pkt.total_chunks = total_chunks;
        def_pkt.chunk_size = chunk_size;
        strcpy(def_pkt.file_name, files[i]);

        int cnt = multicast_send(m, &def_pkt, sizeof(file_defn_packet_t));
        printf("multicast sending definition packet of size %d\n", cnt);

        int seq = 0;
        int bytes_read;
        char *buffer = malloc(chunk_size);
        int done_reading = 0;
        while (!done_reading) {
            if ((bytes_read = fread(buffer, 1, chunk_size, file)) > 0) { // Chunks left to send, then send
                data_packet_t *data_pkt = malloc(sizeof(data_packet_t) + bytes_read);
                data_pkt-> packet_type = 2;
                data_pkt->file_id = i;
                data_pkt->seq_num = seq++;
                data_pkt->checksum = compute_checksum(buffer, bytes_read);
                
                data_pkt->data_size = bytes_read;
                memcpy(data_pkt->data, buffer, bytes_read);

                int snd_cnt = multicast_send(m, data_pkt, sizeof(data_packet_t) + bytes_read);
                printf("Multicast sending data packet of size %d\n", bytes_read);
                free(data_pkt);
            } else { // Read the current file, listen for retransmission requests
                // Send end packet
                end_packet_t end_pkt;
                end_pkt.packet_type = 4;
                end_pkt.file_id = i;
                multicast_send(m, &end_pkt, sizeof(end_packet_t));

                retrans_recv(m, 1, i, filename);  // Listen to retransmission requests until a sender 
                                    // has completed a round of retransmissions.
            }
             
            // RECEIVE (momentary polling for retrans req)
            retrans_recv(m, 0, -1, -1);
        }
        



        fclose(file);
        free(buffer);
    }
    // Shutdown the receiver/cleanup code
    // multicast_destroy(m);
    // return 0;

}