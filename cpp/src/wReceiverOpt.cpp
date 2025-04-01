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
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "../common/PacketHeader.hpp"
#include "../common/Crc32.hpp"

constexpr int MAX_PACKET_SIZE = 1472;

void logPacket(std::ofstream& log, const PacketHeader& header) {
    log << ntohl(header.type) << " " << ntohl(header.seqNum) << " " << ntohl(header.length) << " " << ntohl(header.checksum) << "\n";
    log.flush();
}

int main(int argc, char* argv[]) {
    cxxopts::Options options("wReceiverOpt", "Optimized WTP Receiver");
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

    spdlog::debug("wReceiverOpt listening on port {}", port);

    bool inConnection = false;
    sockaddr_in activeSender{};
    socklen_t senderLen = sizeof(activeSender);
    uint32_t expectedSeqNum = 0;
    int fileIndex = 0;

    char buffer[MAX_PACKET_SIZE];
    std::map<uint32_t, std::vector<char>> bufferMap;
    std::ofstream outputFile;

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

        if (header.type == 0) {  // START
            if (!inConnection) {
                spdlog::debug("Received START packet with seqNum {}", header.seqNum);
                logPacket(logFile, header);

                inConnection = true;
                expectedSeqNum = 0;
                bufferMap.clear();

                std::string filename = outputDir + "/FILE-" + std::to_string(fileIndex++) + ".out";
                outputFile.open(filename, std::ios::binary);
                if (!outputFile) {
                    std::cerr << "Failed to open output file\n";
                    return 1;
                }

                PacketHeader ack{};
                ack.type = htonl(3);
                ack.seqNum = htonl(header.seqNum);
                ack.length = htonl(0);
                ack.checksum = htonl(0);
                sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
                spdlog::debug("Sent ACK for START");
                logPacket(logFile, ack);
            }
        }

        else if (header.type == 2 && inConnection) {  // DATA
            if (len != sizeof(PacketHeader) + header.length) continue;

            uint32_t calcChecksum = crc32(reinterpret_cast<uint8_t*>(buffer + sizeof(PacketHeader)), header.length);
            if (calcChecksum != header.checksum) {
                spdlog::debug("Dropped DATA seqNum {} due to checksum mismatch", header.seqNum);
                continue;
            }

            spdlog::debug("Received DATA packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                header.type, header.seqNum, header.length, header.checksum);
            logPacket(logFile, header);

            uint32_t seqNum = header.seqNum;

            if (seqNum < expectedSeqNum || seqNum >= expectedSeqNum + windowSize) {
                spdlog::debug("DATA seqNum {} outside window or duplicate", seqNum);
            } else if (bufferMap.find(seqNum) == bufferMap.end()) {
                bufferMap[seqNum] = std::vector<char>(buffer + sizeof(PacketHeader), buffer + sizeof(PacketHeader) + header.length);
            }

            // Write and slide window
            while (bufferMap.find(expectedSeqNum) != bufferMap.end()) {
                outputFile.write(bufferMap[expectedSeqNum].data(), bufferMap[expectedSeqNum].size());
                bufferMap.erase(expectedSeqNum);
                ++expectedSeqNum;
            }

            // Individual ACK
            PacketHeader ack{};
            ack.type = htonl(3);
            ack.seqNum = htonl(seqNum);  // Not cumulative!
            ack.length = htonl(0);
            ack.checksum = htonl(0);
            sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
            spdlog::debug("Sent ACK packet: type = {}, seqNum = {}, length = {}, checkSum = {} ", 
                ntohl(ack.type), ntohl(ack.seqNum), ntohl(ack.length), ntohl(ack.checksum));
  
            logPacket(logFile, ack);
        }

        else if (header.type == 1 && inConnection) {  // END
            if (crc32(nullptr, 0) != header.checksum) {
                spdlog::debug("Dropped END due to checksum mismatch");
                continue;
            }

            logPacket(logFile, header);

            for (uint32_t i = expectedSeqNum; bufferMap.find(i) != bufferMap.end(); ++i) {
                outputFile.write(bufferMap[i].data(), bufferMap[i].size());
                bufferMap.erase(i);
            }

            outputFile.close();
            spdlog::debug("Completed file write. Resetting receiver state.");

            PacketHeader ack{};
            ack.type = htonl(3);
            ack.seqNum = htonl(header.seqNum);
            ack.length = htonl(0);
            ack.checksum = htonl(0);
            sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr*)&activeSender, senderLen);
            logPacket(logFile, ack);

            inConnection = false;
            expectedSeqNum = 0;
            bufferMap.clear();
        }
    }

    close(sockfd);
    logFile.close();
    return 0;
}
