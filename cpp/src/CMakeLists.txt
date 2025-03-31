# Create your own CMakeLists.txt file here that will build your project, based 
# on the directory structure you choose. You can use the CMakeLists.txt files
# we provided in the src directory of Project 1 and Project 2 as a starting point.

# You will also have to add a CMakelists.txt file in the common/ directory. 

# Running the standard CMake build sequence should result in the following executables:
#   wSender
#   wReceiver
#   wSenderOpt
#   wReceiverOpt


# Add the common directory for shared headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/common)

# Build wSender
add_executable(wSender wSender.cpp)
target_link_libraries(wSender PRIVATE spdlog::spdlog cxxopts::cxxopts Boost::regex)

# Build wReceiver
add_executable(wReceiver wReceiver.cpp)
target_link_libraries(wReceiver PRIVATE spdlog::spdlog cxxopts::cxxopts Boost::regex)

# Build wSenderOpt (stub for now)
add_executable(wSenderOpt wSenderOpt.cpp)
target_link_libraries(wSenderOpt PRIVATE spdlog::spdlog cxxopts::cxxopts Boost::regex)

# Build wReceiverOpt (stub for now)
add_executable(wReceiverOpt wReceiverOpt.cpp)
target_link_libraries(wReceiverOpt PRIVATE spdlog::spdlog cxxopts::cxxopts Boost::regex)


