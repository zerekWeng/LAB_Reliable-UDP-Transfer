// client.cpp
// UDP Reliable File Transfer (sender)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std;

const int BIG_CHUNK_SIZE = 1024 * 1024 * 500;  // 500 MB

const int HOW_MANY_TIMES_TO_SEND = 1;
const int BUFFER_SIZE = 4096;
const int DEFAULT_TIMEOUT = 400;  // default to approx 1 RTT
const int WINDOW_SIZE = 5;        // MB

int tcp_socket_pool[10];
int udp_socket_pool[10];

struct sockaddr_in client_addr_pool[10];
struct sockaddr_in server_addr_pool[10];

struct sockaddr_in tcp_client_addr_pool[10];
struct sockaddr_in tcp_server_addr_pool[10];

int server_len_pool[10];
int client_len_pool[10];

int udp_socket, tcp_socket;
struct sockaddr_in client_addr, server_addr;
struct sockaddr_in tcp_server_addr, tcp_client_addr;
int server_len, client_len;
int tcp_server_len;

struct Packet {
    int seq_num;
    size_t size;
    bool file_name_flag;
    bool ackFlag;
    char data[BUFFER_SIZE];
} udp_segment;

int get_file_size(ifstream& in) {
    in.seekg(0, ios::end);
    int size = in.tellg();
    in.seekg(0, ios::beg);
    return size;
}

void udp_send_multiple(const struct Packet* udp_segment, int times, int socket,
                       struct sockaddr_in* server_addr, int server_len) {
    for (int i = 0; i < times; i++) {
        if (sendto(socket, udp_segment, sizeof(*udp_segment), 0,
                   (const struct sockaddr*)server_addr, server_len) < 0) {
            perror("sendto failed");
            exit(1);
        }
    }
}

void mt_send_chunk(vector<Packet>& pool, int tcp_socket, int udp_socket,
                   int total_number_of_packets, int start, int end) {
    cout << "MT_send" << endl;
    Packet udp_segment;
    Packet tcp_segment;
    vector<int> missing_packets;
    int retries = 0;

    // send file using UDP
    while (true) {
        // send packets over UDP
        for (int i = start; i < end; i++) {
            // send one packet multiple times to ensure delivery
            if (pool[i].ackFlag) {
                continue;
            } else {
                // cout << "sending packet " << i << endl;
                udp_segment = pool[i];
                udp_segment.seq_num = i;
                udp_segment.ackFlag = false;
                udp_send_multiple(&udp_segment, HOW_MANY_TIMES_TO_SEND,
                                  udp_socket, &server_addr_pool[0],
                                  server_len_pool[0]);
                // cout<<"sent packet "<<i<<" from "<<start<<" to
                // "<<end<<endl;
                pool[i].ackFlag = true;
            }
        }
        // send FIN packet
        for (int i = 0; i < 5; i++) {
            udp_segment.seq_num = -1;
            udp_segment.ackFlag = false;
            udp_send_multiple(&udp_segment, HOW_MANY_TIMES_TO_SEND * 3,
                              udp_socket, &server_addr_pool[0],
                              server_len_pool[0]);
        }

        // receive missing packet list over TCP
        // the list is a giant buffer of integers
        char buffer[total_number_of_packets *
                    sizeof(int)];  // can't lose too much packets
    timeout:
        memset(buffer, 0, total_number_of_packets * sizeof(int));

        // receive missing packet list over UDP
        int n = recvfrom(udp_socket, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)&server_addr_pool[0],
                         (socklen_t*)&server_len_pool[0]);

        if (n == 0) break;
        if (n < 0) {
            // timeout, resend fin packet
            udp_send_multiple(&udp_segment, HOW_MANY_TIMES_TO_SEND * 3,
                              udp_socket, &server_addr_pool[0],
                              server_len_pool[0]);
            cout << "timeout" << endl;
            retries++;
            bool allAcknowledged =
                std::all_of(pool.begin() + start, pool.begin() + end,
                            [](const Packet& p) { return p.ackFlag; });

            if (allAcknowledged && retries > 5) {
                goto end;
            } else {
                goto timeout;
            }
        } else {
            int* missing_packets_ptr = (int*)buffer;
            for (int i = 0; i < n / sizeof(int); i++) {
                if (missing_packets_ptr[i] < end &&
                    missing_packets_ptr[i] >= start) {
                    pool[missing_packets_ptr[i]].ackFlag = false;
                } else if (missing_packets_ptr[i] < start ||
                           missing_packets_ptr[i] >= end + end - start) {
                    continue;
                } else {
                    cout << "missing packet out of window: "
                         << missing_packets_ptr[i] << endl;
                    goto end;
                }
            }
            retries = 0;
        }
    }
end:
    return;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        perror("usage: ./client [address] [port_tcp] [port_udp] [filename]");
        exit(1);
    }

    string address = argv[1];
    int port_tcp = atoi(argv[2]);
    int port_udp = atoi(argv[3]);
    string filename = argv[4];

    int port_tcp_pool[10];
    int port_udp_pool[10];

    for (int i = 0; i < 10; i++) {
        port_tcp_pool[i] = port_tcp + i;
        port_udp_pool[i] = port_udp + i;
    }

    ifstream in(filename, std::ios::binary);
    if (!in) {
        perror("file not found!");
        exit(1);
    }

    // ----------------- setup sockets/addresses -----------------

    // initialize socket pools
    for (int i = 0; i < 1; i++) {
        tcp_socket_pool[i] = socket(AF_INET, SOCK_STREAM, 0);
        udp_socket_pool[i] = socket(AF_INET, SOCK_DGRAM, 0);
    }

    // initialize address pools
    for (int i = 0; i < 1; i++) {
        memset(&client_addr_pool[i], 0, sizeof(client_addr_pool[i]));
        client_addr_pool[i].sin_family = AF_INET;
        client_addr_pool[i].sin_port = htons(0);
        client_addr_pool[i].sin_addr.s_addr = INADDR_ANY;
        if (bind(udp_socket_pool[i],
                 (const struct sockaddr*)&client_addr_pool[i],
                 sizeof(client_addr_pool[i])) < 0) {
            perror("bind failed");
            exit(1);
        }
        client_len_pool[i] = sizeof(client_addr_pool[i]);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = DEFAULT_TIMEOUT * 1000;

        // set UDP receive timeout
        setsockopt(udp_socket_pool[i], SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&tv, sizeof tv);

        memset(&tcp_client_addr_pool[i], 0, sizeof(tcp_client_addr_pool[i]));
        tcp_client_addr_pool[i].sin_family = AF_INET;
        tcp_client_addr_pool[i].sin_port = htons(0);
        tcp_client_addr_pool[i].sin_addr.s_addr = INADDR_ANY;
        if (bind(tcp_socket_pool[i],
                 (const struct sockaddr*)&tcp_client_addr_pool[i],
                 sizeof(tcp_client_addr_pool[i])) < 0) {
            perror("bind failed");
            exit(1);
        }

        // set TCP receive timeout
        setsockopt(tcp_socket_pool[i], SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&tv, sizeof tv);

        memset(&server_addr_pool[i], 0, sizeof(server_addr_pool[i]));
        server_addr_pool[i].sin_family = AF_INET;
        server_addr_pool[i].sin_port = htons(port_udp_pool[i]);
        if (inet_pton(AF_INET, address.c_str(),
                      &server_addr_pool[i].sin_addr) <= 0) {
            perror("invalid address");
            exit(1);
        }
        server_len_pool[i] = sizeof(server_addr_pool[i]);

        memset(&tcp_server_addr_pool[i], 0, sizeof(tcp_server_addr_pool[i]));
        tcp_server_addr_pool[i].sin_family = AF_INET;
        tcp_server_addr_pool[i].sin_port = htons(port_tcp_pool[i]);
        if (inet_pton(AF_INET, address.c_str(),
                      &tcp_server_addr_pool[i].sin_addr) <= 0) {
            perror("invalid address");
            exit(1);
        }
    }

    // ----------------- setup connection -----------------
    // connect to server (first socket only to send file name and file size)
    if (connect(tcp_socket_pool[0], (struct sockaddr*)&tcp_server_addr_pool[0],
                sizeof(tcp_server_addr_pool[0])) < 0) {
        perror("connection failed");
        exit(1);
    }

    int number_of_connections = 1;
    int file_size = get_file_size(in);

    cout << "file size: " << file_size << endl;

    int total_number_of_packets = file_size % BUFFER_SIZE == 0
                                      ? file_size / BUFFER_SIZE
                                      : file_size / BUFFER_SIZE + 1;

    // ----------------- setup file -----------------

    int total_big_chunks = file_size % BIG_CHUNK_SIZE == 0
                               ? file_size / BIG_CHUNK_SIZE
                               : file_size / BIG_CHUNK_SIZE + 1;

    Packet tcp_segment;
    tcp_segment.file_name_flag = true;
    memset(tcp_segment.data, 0, BUFFER_SIZE);
    memcpy(tcp_segment.data, filename.c_str(), filename.size());

    tcp_segment.size = file_size;

    auto start = std::chrono::high_resolution_clock::now();

    if (send(tcp_socket_pool[0], &tcp_segment, sizeof(tcp_segment), 0) < 0) {
        perror("send failed");
        exit(1);
    }  // socket 0 will be on no matter what

    for (int i = 0; i < total_big_chunks; i++) {
        // setup the total pool of packets
        vector<Packet> pool;

        // calculate start and end points for the big chunk
        int bigChunkStart = i * BIG_CHUNK_SIZE;
        int bigChunkEnd = std::min(bigChunkStart + BIG_CHUNK_SIZE, file_size);
        int bigChunkSize = bigChunkEnd - bigChunkStart;
        int numberOfPackets = bigChunkSize / BUFFER_SIZE +
                              (bigChunkSize % BUFFER_SIZE != 0 ? 1 : 0);

        // set the read position to the start of the current big chunk
        in.seekg(bigChunkStart);

        // load up the pool with data of the big chunk
        for (int k = 0; k < numberOfPackets; ++k) {
            Packet packet;
            packet.file_name_flag = false;
            packet.ackFlag = false;
            in.read(packet.data, BUFFER_SIZE);
            packet.size = in.gcount();
            pool.push_back(packet);
        }

        cout << "number of packets: " << pool.size() << endl;

        // ----------------- send file -----------------
        int seq_num = 0;
        cout << "size of the last packet: " << pool.back().size << endl;

        // send file using UDP
        int chunk = 1024 * 1024 * WINDOW_SIZE / BUFFER_SIZE;
        int start_index = 0;
        int total_chunks = numberOfPackets % chunk == 0
                               ? numberOfPackets / chunk
                               : numberOfPackets / chunk + 1;
        int end_index = std::min(chunk, numberOfPackets);

        for (int j = 0; j < total_chunks; j++) {
            cout << "sending chunk " << j << endl;
            mt_send_chunk(pool, tcp_socket_pool[0], udp_socket_pool[0],
                          numberOfPackets, start_index, end_index);
            cout << "chunk " << j << " sent" << endl;
            start_index = end_index;
            end_index = std::min(end_index + chunk, numberOfPackets);
        }
    }

    // stop timer
    auto stop = std::chrono::high_resolution_clock::now();

    // calculate throughput and print
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    double throughput =
        (double)file_size * 8 / duration.count() * 1000 / 1024 / 1024;  // Mbps

    std::cout << "Throughput: " << throughput << " Mbps" << std::endl;

    in.close();
    cout << "File sent\n";

    for (int i = 0; i < 10; i++) {
        close(tcp_socket_pool[i]);
        close(udp_socket_pool[i]);
    }
    return 0;
}