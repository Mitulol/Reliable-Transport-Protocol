#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>   // for basic_logger_mt
#include <spdlog/sinks/stdout_color_sinks.h> // for stdout_color_mt

#include "../common/PacketHeader.hpp"
#include "../common/Crc32.hpp"

constexpr int MAX_PACKET_SIZE = 1472;
constexpr int HEADER_SIZE = sizeof(PacketHeader);
constexpr int MAX_CHUNK_SIZE = MAX_PACKET_SIZE - HEADER_SIZE;

std::vector<std::vector<char>> readChunks(const std::string &inputFile) {
    std::ifstream file(inputFile, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open input file");
    }

    std::vector<std::vector<char>> chunks;
    while (!file.eof()) {
        std::vector<char> buffer(MAX_CHUNK_SIZE);
        file.read(buffer.data(), MAX_CHUNK_SIZE);
        size_t bytesRead = file.gcount();
        buffer.resize(bytesRead);
        if (bytesRead > 0) chunks.push_back(std::move(buffer));
    }

    return chunks;
}

void logPacket(std::ofstream& log, const PacketHeader& header) {
    log << ntohl(header.type) << " " << ntohl(header.seqNum) << " " << ntohl(header.length) << " " << ntohl(header.checksum) << "\n";
    log.flush();
}


int main(int argc, char* argv[]) {
    cxxopts::Options options("wSender", "WTP Sender");
    options.add_options()
        ("h,hostname", "Receiver IP", cxxopts::value<std::string>())
        ("p,port", "Receiver Port", cxxopts::value<int>())
        ("w,window-size", "Window Size", cxxopts::value<int>())
        ("i,input-file", "Input File", cxxopts::value<std::string>())
        ("o,output-log", "Log File", cxxopts::value<std::string>())
        ("help", "Print help");

    auto result = options.parse(argc, argv);
    if (result.count("help") || !result.count("hostname") || !result.count("port") ||
        !result.count("window-size") || !result.count("input-file") || !result.count("output-log")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    std::string hostname = result["hostname"].as<std::string>();
    int port = result["port"].as<int>();
    int windowSize = result["window-size"].as<int>();
    std::string inputFile = result["input-file"].as<std::string>();
    std::string logPath = result["output-log"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    std::ofstream logFile(logPath);
    if (!logFile) {
        std::cerr << "Failed to open log file\n";
        return 1;
    }


    // Read input file and break into chunks
    std::vector<std::vector<char>> chunks;
    try {
        chunks = readChunks(inputFile);
        spdlog::debug("Read {} chunks from input file", chunks.size());
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    // Step 3: Create UDP socket and resolve receiver address
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    struct sockaddr_in receiverAddr;
    memset(&receiverAddr, 0, sizeof(receiverAddr));
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, hostname.c_str(), &receiverAddr.sin_addr) <= 0) {
        std::cerr << "Invalid IP address: " << hostname << std::endl;
        close(sockfd);
        return 1;
    }

    spdlog::debug("UDP socket created and receiver address set to {}:{}", hostname, port);


    srand(time(nullptr));
    uint32_t startSeqNum = rand(); // Random initial sequence number

    PacketHeader startHeader;
    startHeader.type = htonl(0); // START
    startHeader.seqNum = htonl(startSeqNum);
    startHeader.length = htonl(0);
    startHeader.checksum = htonl(0);

    bool ackReceived = false;
    char recvBuffer[MAX_PACKET_SIZE];
    socklen_t addrLen = sizeof(receiverAddr);

    while (!ackReceived) {
        // Send START packet
        sendto(sockfd, &startHeader, sizeof(startHeader), 0,
               (struct sockaddr*)&receiverAddr, sizeof(receiverAddr));
        
        // spdlog::debug("{} {} {} {}", startHeader.type, startHeader.seqNum, startHeader.length, startHeader.checksum);
        logPacket(logFile, startHeader);
        spdlog::debug("Sent START packet with seqNum = {}", startSeqNum);

        // Set timeout for 500ms
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500 * 1000; // 500ms

        int ready = select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);

        if (ready > 0 && FD_ISSET(sockfd, &readfds)) {
            ssize_t recvLen = recvfrom(sockfd, recvBuffer, MAX_PACKET_SIZE, 0,
                                       (struct sockaddr*)&receiverAddr, &addrLen);

            if (recvLen >= sizeof(PacketHeader)) {
                PacketHeader* ack = reinterpret_cast<PacketHeader*>(recvBuffer);
                if (ntohl(ack->type) == 3 && ntohl(ack->seqNum) == startSeqNum) {
                    // spdlog::debug("{} {} {} {}", ack->type, ack->seqNum, ack->length, ack->checksum);
                    logPacket(logFile, *ack);
                    spdlog::debug("Received ACK for START packet.");
                    ackReceived = true;
                    break;
                } else {
                    spdlog::warn("Received unexpected packet: type = {}, seqNum = {}", ntohl(ack->type), ntohl(ack->seqNum));
                }
            }
        } else {
            spdlog::warn("Timeout waiting for ACK. Retrying START...");
        }
    }

    if (!ackReceived) {
        std::cerr << "Failed to receive ACK for START after multiple attempts.\n";
        close(sockfd);
        return 1;
    }


    uint32_t nextSeqNum = 0;  // First data seqNum
    uint32_t baseSeqNum = 0;      // Lowest unACKed seqNum

    std::unordered_map<uint32_t, std::vector<char>> inFlightPackets;  // seqNum -> data
    std::unordered_map<uint32_t, PacketHeader> inFlightHeaders;       // seqNum -> header

    // size_t chunkIndex = 0;

    fd_set readfds;
    struct timeval timeout;
    char ackBuffer[MAX_PACKET_SIZE];

    while (baseSeqNum < chunks.size()) {
        // Step 1: Send as many packets as the window allows
        while (nextSeqNum < baseSeqNum + windowSize && nextSeqNum < chunks.size()) {

            uint32_t payloadLen = chunks[nextSeqNum].size();
            uint32_t checksum = crc32(reinterpret_cast<uint8_t*>(chunks[nextSeqNum].data()), payloadLen);

            PacketHeader header;
            header.type = htonl(2); // DATA
            header.seqNum = htonl(nextSeqNum);
            header.length = htonl(payloadLen);
            header.checksum = htonl(checksum);

            // Create full packet (header + data)
            std::vector<char> packet(sizeof(PacketHeader) + payloadLen);
            std::memcpy(packet.data(), &header, sizeof(PacketHeader));
            std::memcpy(packet.data() + sizeof(PacketHeader), chunks[nextSeqNum].data(), payloadLen);

            // Send packet
            sendto(sockfd, packet.data(), packet.size(), 0,
                (struct sockaddr*)&receiverAddr, sizeof(receiverAddr));

            spdlog::debug("Sent DATA packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                ntohl(header.type), ntohl(header.seqNum), ntohl(header.length), ntohl(header.checksum));
            logPacket(logFile, header);

            // Store for retransmission
            inFlightPackets[nextSeqNum] = chunks[nextSeqNum];
            inFlightHeaders[nextSeqNum] = header;

            ++nextSeqNum;
            // ++chunkIndex;
        }

        // Step 2: Set 500ms timeout and wait for ACKs
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);

        timeout.tv_sec = 0;
        timeout.tv_usec = 500 * 1000;

        int ready = select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
        bool ackMovedWindow = false;

        if (ready > 0 && FD_ISSET(sockfd, &readfds)) {
            ssize_t ackLen = recvfrom(sockfd, ackBuffer, MAX_PACKET_SIZE, 0,
                                    (struct sockaddr*)&receiverAddr, &addrLen);

            if (ackLen >= sizeof(PacketHeader)) {
                PacketHeader* ack = reinterpret_cast<PacketHeader*>(ackBuffer);
                if (ntohl(ack->type) == 3) {
                    logPacket(logFile, *ack);
                    spdlog::debug("Received ACK packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                        ntohl(ack->type), ntohl(ack->seqNum), ntohl(ack->length), ntohl(ack->checksum));

                    uint32_t ackNum = ntohl(ack->seqNum);

                    if (ackNum > baseSeqNum) {
                        // Slide window forward
                        for (uint32_t s = baseSeqNum; s < ackNum; ++s) {
                            inFlightPackets.erase(s);
                            inFlightHeaders.erase(s);
                        }
                        baseSeqNum = ackNum;
                        ackMovedWindow = true;
                        // nextSeqNum = baseSeqNum;
                    }
                }
            }
        }

        // Step 3: If no ACKs moved the window forward, retransmit all
        if (!ackMovedWindow) {
            for (auto& [seq, data] : inFlightPackets) {
                PacketHeader& header = inFlightHeaders[seq];

                uint32_t length = ntohl(header.length);
                std::vector<char> packet(sizeof(PacketHeader) + length);
                std::memcpy(packet.data(), &header, sizeof(PacketHeader));
                std::memcpy(packet.data() + sizeof(PacketHeader), data.data(), length);

                sendto(sockfd, packet.data(), packet.size(), 0,
                    (struct sockaddr*)&receiverAddr, sizeof(receiverAddr));

                spdlog::debug("Sent DATA packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                    ntohl(header.type), ntohl(header.seqNum), ntohl(header.length), ntohl(header.checksum));
                logPacket(logFile, header);
                spdlog::debug("Retransmitted packet {}", seq);
            }
        }
    }

    uint32_t endSeqNum = startSeqNum;

    PacketHeader endHeader;
    endHeader.type = htonl(1); // END
    endHeader.seqNum = htonl(endSeqNum);
    endHeader.length = htonl(0);
    endHeader.checksum = htonl(0);

    ackReceived = false;

    while (!ackReceived) {
        sendto(sockfd, &endHeader, sizeof(endHeader), 0,
               (struct sockaddr*)&receiverAddr, sizeof(receiverAddr));
    
        logPacket(logFile, endHeader);
        spdlog::debug("Sent END packet with seqNum = {}", endSeqNum);
    
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 500 * 1000;
    
        int ready = select(sockfd + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready > 0 && FD_ISSET(sockfd, &readfds)) {
            ssize_t recvLen = recvfrom(sockfd, recvBuffer, MAX_PACKET_SIZE, 0,
                                       (struct sockaddr*)&receiverAddr, &addrLen);
    
            if (recvLen >= sizeof(PacketHeader)) {
                PacketHeader* ack = reinterpret_cast<PacketHeader*>(recvBuffer);
                if (ntohl(ack->type) == 3 && ntohl(ack->seqNum) == endSeqNum) {
                    logPacket(logFile, *ack);
                    spdlog::debug("Received ACK for END packet.");
                    ackReceived = true;
                    break;
                } else {
                    spdlog::warn("Received unexpected packet: type = {}, seqNum = {}", ntohl(ack->type), ntohl(ack->seqNum));
                }
            }
        } else {
            spdlog::warn("Timeout waiting for ACK of END. Retrying...");
        }
    }
    
    if (!ackReceived) {
        std::cerr << "Failed to receive ACK for END after multiple attempts.\n";
        close(sockfd);
        return 1;
    }

    // Close connection
    close(sockfd);
    logFile.close();
    return 0;
}
