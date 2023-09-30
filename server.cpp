// server.cpp
// UDP Reliable File Transfer (receiver)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>
#include <vector>
using namespace std;

const int BIG_CHUNK_SIZE = 1024 * 1024 * 500;  // 500 MB

const int BUFFER_SIZE = 4096;
const int DEFAULT_TIMEOUT = 400;
const int WINDOW_SIZE = 5;  // in MB

int tcp_socket_pool[10];
int udp_socket_pool[10];

struct sockaddr_in client_addr_pool[10];
struct sockaddr_in server_addr_pool[10];

struct sockaddr_in tcp_client_addr_pool[10];
struct sockaddr_in tcp_server_addr_pool[10];

int server_len_pool[10];
int client_len_pool[10];

struct Packet {
    int seq_num;
    size_t size;
    bool file_name_flag;
    bool ackFlag;
    char data[BUFFER_SIZE];
} udp_segment;

void mt_receive_chunk(Packet* packets, int udp_socket, int tcp_socket,
                      struct sockaddr_in* client_addr, int total_packet_num,
                      int thread_id, vector<int>& missing_pkt_list, int start,
                      int end) {
    cout << "thread " << thread_id << " started" << endl;

    int client_len = sizeof(*client_addr);

    // Declaring udp_segment
    Packet udp_segment;
    Packet tcp_segment;

    int counter = 0;

    // Main receive loop
    while (true) {
        // Receive packet
        int retries = 0;
        while (true) {
            // Receive packet
            memset(&udp_segment, 0, sizeof(udp_segment));
            int n = recvfrom(udp_socket, &udp_segment, sizeof(udp_segment), 0,
                             (struct sockaddr*)client_addr,
                             (socklen_t*)&client_len);

            // If the packet is the FIN packet, break
            if (udp_segment.seq_num == -1) {
                retries = 0;
                break;
            } else {
                retries = 0;
                if (udp_segment.seq_num < start || udp_segment.seq_num >= end) {
                    continue;
                } else {
                    packets[udp_segment.seq_num] = udp_segment;
                }
                // cout << "Received packet " << udp_segment.seq_num
                //      << " from "<<start<<" to "<<end<<endl;
            }
        }

        // Check for missing packets
        missing_pkt_list.clear();
        for (int i = start; i < end; i++) {
            if (packets[i].seq_num != i) {
                missing_pkt_list.push_back(i);
            }
        }

        // if ((counter % 50)) {
        //     cout << "Number of missing packets: " << missing_pkt_list.size()
        //          << endl;
        // }

        // If there are no missing packets, break
        if (missing_pkt_list.empty()) {
            break;
        }

        // // Send the buffer over TCP
        // if (send(tcp_socket, missing_pkt_list.data(),
        //          sizeof(int) * missing_pkt_list.size(), 0) < 0) {
        //     perror("TCP Send failed");
        //     exit(EXIT_FAILURE);
        // }

        // Send the buffer over UDP
        if (sendto(udp_socket, missing_pkt_list.data(),
                   sizeof(int) * missing_pkt_list.size(), 0,
                   (struct sockaddr*)client_addr, client_len) < 0) {
            perror("UDP Send failed");
            exit(EXIT_FAILURE);
        }

        counter++;
    }

    // cout << "UDP receive complete chunk: " << thread_id << endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: ./server <tcp_port> <udp_port>\n");
        exit(EXIT_FAILURE);
    }
    int tcp_port = atoi(argv[1]);
    int udp_port = atoi(argv[2]);

    // ------------------- setup sockets/addresses -------------------
    // setup port number
    int port_tcp_pool[10];
    int port_udp_pool[10];

    for (int i = 0; i < 10; i++) {
        port_tcp_pool[i] = tcp_port + i;
        port_udp_pool[i] = udp_port + i;
    }

    // initialize socket pools
    for (int i = 0; i < 10; i++) {
        tcp_socket_pool[i] = socket(AF_INET, SOCK_STREAM, 0);
        udp_socket_pool[i] = socket(AF_INET, SOCK_DGRAM, 0);
    }

    // initialize server addresses
    for (int i = 0; i < 10; i++) {
        memset(&server_addr_pool[i], 0, sizeof(server_addr_pool[i]));
        server_addr_pool[i].sin_family = AF_INET;  // IPv4
        server_addr_pool[i].sin_addr.s_addr = INADDR_ANY;
        server_addr_pool[i].sin_port = htons(port_udp_pool[i]);
        if (bind(udp_socket_pool[i],
                 (const struct sockaddr*)&server_addr_pool[i],
                 sizeof(server_addr_pool[i])) < 0) {
            perror("Bind failed");
            exit(EXIT_FAILURE);
        }

        memset(&tcp_server_addr_pool[i], 0, sizeof(tcp_server_addr_pool[i]));
        tcp_server_addr_pool[i].sin_family = AF_INET;  // IPv4
        tcp_server_addr_pool[i].sin_addr.s_addr = INADDR_ANY;
        tcp_server_addr_pool[i].sin_port = htons(port_tcp_pool[i]);
        if (bind(tcp_socket_pool[i],
                 (const struct sockaddr*)&tcp_server_addr_pool[i],
                 sizeof(tcp_server_addr_pool[i])) < 0) {
            perror("Bind failed");
            exit(EXIT_FAILURE);
        }
    }

    // listen for incoming connections
    // turn on listening for tcp socket 0 first to receive file name and file
    // size
    if (listen(tcp_socket_pool[0], 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    // accept incoming connection
    int tcp_client_len = sizeof(tcp_client_addr_pool[0]);
    tcp_socket_pool[0] =
        accept(tcp_socket_pool[0], (struct sockaddr*)&tcp_client_addr_pool[0],
               (socklen_t*)&tcp_client_len);

    // extract client's address and port
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(tcp_client_addr_pool[0].sin_addr), client_ip,
              INET_ADDRSTRLEN);
    int client_port = ntohs(tcp_client_addr_pool[0].sin_port);
    printf("Client %s:%d connected\n", client_ip, client_port);

    // Receive file name and file size over TCP connection 0
    // no matter how many connections there are, the file name and file size
    // will be sent over the first connection
    string file_name;
    size_t file_size;
    Packet tcp_segment;
    memset(&tcp_segment, 0, sizeof(tcp_segment));
    if (recv(tcp_socket_pool[0], &tcp_segment, sizeof(tcp_segment), 0) < 0) {
        perror("Receive failed");
        exit(EXIT_FAILURE);
    }

    file_name = string(tcp_segment.data);
    file_size = tcp_segment.size;
    printf("File name: %s\n", file_name.c_str());
    printf("File size: %lu\n", file_size);

    memset(&tcp_segment, 0, sizeof(tcp_segment));

    int number_of_connections = 1;

    // set big chunks
    int big_chunks = file_size % BIG_CHUNK_SIZE == 0
                         ? file_size / BIG_CHUNK_SIZE
                         : file_size / BIG_CHUNK_SIZE + 1;

    int total_packet_num = (file_size % BUFFER_SIZE) > 0
                               ? file_size / BUFFER_SIZE + 1
                               : file_size / BUFFER_SIZE;

    ofstream file(file_name, ios::out | ios::binary);

    for (int i = 0; i < big_chunks; i++) {
        vector<int> missing_pkt_list;

        // receive file over UDP
        // seq_num is the sequence number of the packet
        // in total, there are file_size/BUFFER_SIZE or file_size/BUFFER_SIZE +
        // 1 packets if a sequence number is missing, the sequence number will
        // be added to missing_pkt_list

        int seq_num = 0;

        // create an array to store the packets
        // the index of the array is the sequence number of the packet
        // the value of the array is the packet
        int bigChunkStart = i * BIG_CHUNK_SIZE;
        int bigChunkEnd =
            std::min<int>(bigChunkStart + BIG_CHUNK_SIZE, file_size);
        int bigChunkSize = bigChunkEnd - bigChunkStart;
        cout << "big chunk " << i << " size: " << bigChunkSize << endl;
        int numberOfPackets = bigChunkSize / BUFFER_SIZE +
                              (bigChunkSize % BUFFER_SIZE == 0 ? 0 : 1);
        Packet* packets = new Packet[numberOfPackets];

        memset(packets, 0, sizeof(Packet) * numberOfPackets);
        // make sure all sequence numbers are initialized to -1
        for (int i = 0; i < numberOfPackets; i++) {
            packets[i].seq_num = -1;
        }

        cout << "Big Chunk " << i << " started" << endl;
        cout << "number of packets: " << numberOfPackets << endl;

        int chunk = 1024 * 1024 * WINDOW_SIZE / BUFFER_SIZE;
        int start_index = 0;
        int total_chunks = numberOfPackets % chunk == 0
                               ? numberOfPackets / chunk
                               : numberOfPackets / chunk + 1;

        cout << "total chunks: " << total_chunks << endl;
        int end_index = std::min(chunk, bigChunkSize);

        for (int j = 0; j < total_chunks; j++) {
            cout << "receiving chunk " << j << endl;
            mt_receive_chunk(packets, udp_socket_pool[0], tcp_socket_pool[0],
                             &client_addr_pool[0], bigChunkSize, j,
                             missing_pkt_list, start_index, end_index);
            start_index = end_index;
            end_index = std::min(end_index + chunk, numberOfPackets);
        }

        // store the packets in the file with filename file_name
        for (int j = 0; j < numberOfPackets; j++) {
            file.write(packets[j].data, packets[j].size);
        }

        delete[] packets;
        missing_pkt_list.clear();
    }

    // for loop to close all sockets
    for (int i = 0; i < 10; i++) {
        close(tcp_socket_pool[i]);
        close(udp_socket_pool[i]);
    }

    file.close();
    return 0;
}
