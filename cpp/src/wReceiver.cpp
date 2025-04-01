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

void logPacket(std::ofstream& log, const PacketHeader& header) {
    log << header.type << " " << header.seqNum << " " << header.length << " " << header.checksum << "\n";
    log.flush();
}

int main(int argc, char* argv[]) {
    cxxopts::Options options("wReceiver", "WTP Receiver");
    options.add_options()
        ("p,port", "Listening port", cxxopts::value<int>())
        ("w,window-size", "Window Size", cxxopts::value<int>())
        ("d,output-dir", "Directory to store output", cxxopts::value<std::string>())
        ("o,output-log", "Log File", cxxopts::value<std::string>())
        ("help", "Print help");

    auto result = options.parse(argc, argv);
    if (result.count("help") || !result.count("port") || !result.count("window-size") ||
        !result.count("output-dir") || !result.count("output-log")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    int port = result["port"].as<int>();
    int windowSize = result["window-size"].as<int>();
    std::string outputDir = result["output-dir"].as<std::string>();
    std::string logPath = result["output-log"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    std::ofstream logFile(logPath);
    if (!logFile) {
        std::cerr << "Failed to open log file\n";
        return 1;
    }

    // Step 1: Create socket and bind
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_in receiverAddr{};
    receiverAddr.sin_family = AF_INET;
    receiverAddr.sin_addr.s_addr = INADDR_ANY;
    receiverAddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&receiverAddr, sizeof(receiverAddr)) < 0) {
        std::cerr << "Bind failed\n";
        return 1;
    }

    spdlog::debug("wReceiver listening on port {}", port);

    // Step 2: Listen for packets
    bool inConnection = false;
    sockaddr_in activeSender{};
    uint32_t expectedSeqNum = 0;
    int fileIndex = 0;

    char buffer[MAX_PACKET_SIZE];
    socklen_t senderLen = sizeof(activeSender);

    // Initialize buffer if needed
    std::map<uint32_t, std::vector<char>> bufferMap;

    while (true) {
        ssize_t len = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                               (struct sockaddr*)&activeSender, &senderLen);

        if (len < sizeof(PacketHeader)) continue;

        PacketHeader* rawHeader = reinterpret_cast<PacketHeader*>(buffer);
        PacketHeader header;
        header.type = ntohl(rawHeader->type);
        header.seqNum = ntohl(rawHeader->seqNum);
        header.length = ntohl(rawHeader->length);
        header.checksum = ntohl(rawHeader->checksum);
        

        if (header.type == 0) {  // START packet
            if (!inConnection) {
                spdlog::debug("Received START packet with seqNum {}", header.seqNum);
                logPacket(logFile, header);

                // Save sender info
                inConnection = true;
                expectedSeqNum = 0;

                // Send ACK for START
                PacketHeader ack{};
                ack.type = htonl(3);
                ack.seqNum = htonl(header.seqNum);
                ack.length = htonl(0);
                ack.checksum = htonl(0);

                sendto(sockfd, &ack, sizeof(ack), 0,
                    (struct sockaddr*)&activeSender, senderLen);
                spdlog::debug("Sent ACK for START");
                logPacket(logFile, ack);
            } else {
                // Ignore START during active connection
                spdlog::debug("Ignored START packet (already in active connection)");
            }
        }

        // Handle DATA
        else if (header.type == 2 && inConnection) {  // DATA packet
            // Check checksum over data portion
            size_t dataLen = header.length;
            if (len != sizeof(PacketHeader) + dataLen) continue;  // malformed, drop
        
            uint32_t calcChecksum = crc32(reinterpret_cast<uint8_t*>(buffer + sizeof(PacketHeader)), dataLen);
            if (calcChecksum != header.checksum) {
                spdlog::debug("Dropped DATA seqNum {} due to checksum mismatch", header.seqNum);
                continue;  // Drop malformed packet, do not ACK or log
            }

            spdlog::debug("Received DATA packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                header.type, header.seqNum, header.length, header.checksum);
            logPacket(logFile, header);
        
            uint32_t seqNum = header.seqNum;

            if (seqNum < expectedSeqNum) {
                spdlog::debug("Duplicate DATA seqNum {}, re-ACKing {}", seqNum, expectedSeqNum);
                
                PacketHeader ack{};
                ack.type = htonl(3);
                ack.seqNum = htonl(expectedSeqNum);
                ack.length = htonl(0);
                ack.checksum = htonl(0);
                sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
                spdlog::debug("Sent ACK packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                    ack.type, ack.seqNum, ack.length, ack.checksum);
                logPacket(logFile, ack);
                continue;
            }
        
            // Outside window: drop and ACK expectedSeqNum
            if (seqNum >= expectedSeqNum + windowSize) {
                PacketHeader ack{};
                ack.type = htonl(3);
                ack.seqNum = htonl(expectedSeqNum);
                ack.length = htonl(0);
                ack.checksum = htonl(0);
                sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
                logPacket(logFile, ack);
                spdlog::debug("Dropped DATA seqNum {} outside window, sent ACK {}", seqNum, expectedSeqNum);
                continue;
            }
        
            // If not already buffered, store it
            if (bufferMap.find(seqNum) == bufferMap.end()) {
                bufferMap[seqNum] = std::vector<char>(buffer + sizeof(PacketHeader), buffer + sizeof(PacketHeader) + dataLen);
                // logPacket(logFile, *header);
            }
        
            // Slide window forward
            while (bufferMap.find(expectedSeqNum) != bufferMap.end()) {
                ++expectedSeqNum;
            }
        
            // Send ACK
            PacketHeader ack{};
            ack.type = htonl(3);
            ack.seqNum = htonl(expectedSeqNum);
            ack.length = htonl(0);
            ack.checksum = htonl(0);
            sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
            logPacket(logFile, ack);
            spdlog::debug("Sent cumulative ACK {}", expectedSeqNum);
        }

        // Handle END Packet
        else if (header.type == 1 && inConnection) {  // END packet
            uint32_t calcChecksum = crc32(nullptr, 0);  // No data to check
            if (calcChecksum != header.checksum) {
                spdlog::debug("Dropped END packet due to checksum mismatch");
                continue;
            }
        
            logPacket(logFile, header);
        
            // Only accept END if all packets up to expectedSeqNum have been received
            if (!bufferMap.empty() && bufferMap.begin()->first != 0) {
                spdlog::debug("Rejecting END: Missing earlier packets");
                continue;
            }
        
            // Write buffered data to disk
            std::string filename = outputDir + "/FILE-" + std::to_string(fileIndex++) + ".out";
            std::ofstream outputFile(filename, std::ios::binary);
            for (uint32_t i = 0; i < expectedSeqNum; ++i) {
                if (bufferMap.find(i) != bufferMap.end()) {
                    outputFile.write(bufferMap[i].data(), bufferMap[i].size());
                } else {
                    spdlog::warn("Missing packet {} during file write", i);
                }
            }
            outputFile.close();
            spdlog::debug("Wrote received file to {}", filename);
        
            // Send ACK for END
            PacketHeader ack{};
            ack.type = htonl(3);
            ack.seqNum = htonl(header.seqNum);
            ack.length = htonl(0);
            ack.checksum = htonl(0);
            sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
            logPacket(logFile, ack);
            spdlog::debug("ACKed END packet, resetting receiver state");
        
            // Reset state
            inConnection = false;
            expectedSeqNum = 0;
            bufferMap.clear();
        }
        
        

    }
    close (sockfd);
    logFile.close();
    return 0;
}
