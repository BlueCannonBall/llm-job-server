#include "Polyweb/polyweb.hpp"
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <openssl/sha.h>
#include <queue>
#include <string>

struct ClientInfo {
    std::string current_job;
    time_t rawtime;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: Too few arguments" << std::endl;
        return 1;
    }
    std::string port = argc >= 4 ? argv[3] : "8000";

    std::ifstream prompts_file(argv[1]);
    std::ofstream responses_file(argv[2]);
    if (!prompts_file.is_open() || !responses_file.is_open()) {
        std::cerr << "Error: Could not open file" << std::endl;
        return 1;
    }

    std::mutex mutex;
    std::queue<std::string> queue;
    for (std::string line; std::getline(prompts_file, line); queue.push(std::move(line))) {}
    prompts_file.close();

    pn::init();
    pn::UniqueSock<pw::Server> server;

    server->route_ws(
        "/",
        pw::WSRoute {
            [&mutex, &queue](pw::Connection& conn, void*) {
                ClientInfo* client_info = new ClientInfo;
                conn.data = client_info;

                std::unique_lock<std::mutex> lock(mutex);
                if (!queue.empty()) {
                    client_info->current_job = queue.front();
                    client_info->rawtime = time(nullptr);
                    queue.pop();
                } else {
                    std::cout << "Finished!" << std::endl;
                    pn::quit();
                    exit(0);
                }
                queue.pop();
                mutex.unlock();

                if (conn.send(pw::WSMessage(client_info->current_job)) == PN_ERROR) {
                    conn.close();
                    return;
                }

                std::cout << "New client connected to job server" << std::endl;
            },
                [&responses_file, &mutex, &queue](pw::Connection& conn, pw::WSMessage message, void*) {
                    if (message.opcode != 1 || message.data.empty()) {
                        conn.close();
                        return;
                    }

                    // Check Proof of Work
                    static_assert(sizeof(unsigned int) >= 4, "unsigned int must be 4 bytes");
                    unsigned int hash[SHA256_DIGEST_LENGTH / sizeof(unsigned int)];
                    SHA256((const unsigned char*) message.data.data(), message.data.size(), (unsigned char*) hash);
#if BYTE_ORDER == LITTLE_ENDIAN
                    if (hash[0] && __builtin_ctz(hash[0]) < 20) {
#elif BYTE_ORDER == BIG_ENDIAN
                    if (hash[0] && __builtin_clz(hash[0]) < 20) {
#else
    #error Unsupported byte order
#endif
                        conn.close();
                        return;
                    }

                    auto client_info = (ClientInfo*) conn.data;

                    pw::QueryParameters query_parameters;
                    query_parameters.parse(message.to_string());

                    pw::QueryParameters::map_type::const_iterator time_it;
                    if ((time_it = query_parameters->find("time")) == query_parameters->end()) {
                        conn.close();
                        return;
                    }

                    time_t rawtime;
                    try {
                        rawtime = std::stoull(time_it->second);
                    } catch (const std::exception& e) {
                        conn.close();
                        return;
                    }
                    if (rawtime < client_info->rawtime) {
                        conn.close();
                        return;
                    }

                    pw::QueryParameters::map_type::const_iterator resp_it;
                    if ((resp_it = query_parameters->find("response")) == query_parameters->end()) {
                        conn.close();
                        return;
                    }

                    if (!resp_it->second.empty()) {
                        responses_file << client_info->current_job << '\t' << resp_it->second << std::endl;
                    }

                    std::unique_lock<std::mutex> lock(mutex);
                    if (!queue.empty()) {
                        client_info->current_job = queue.front();
                        client_info->rawtime = time(nullptr);
                        queue.pop();
                    } else {
                        std::cout << "Finished!" << std::endl;
                        pn::quit();
                        exit(0);
                    }
                    lock.unlock();

                    if (conn.send(pw::WSMessage(client_info->current_job)) == PN_ERROR) {
                        conn.close();
                        return;
                    }
                },
                [&mutex, &queue](pw::Connection& conn, uint16_t status_code, const std::string& reason, bool clean, void*) {
                    auto client_info = (ClientInfo*) conn.data;
                    mutex.lock();
                    queue.push(client_info->current_job);
                    mutex.unlock();
                    delete client_info;

                    std::cout << "Client " << (clean ? "cleanly" : "uncleanly") << " disconnected from job server" << std::endl;
                },
        });

    if (server->bind("0.0.0.0", port) == PN_ERROR) {
        std::cerr << "Error: " << pn::universal_strerror() << std::endl;
        return 1;
    }

    std::cout << "LLM Job Server listening on port " << port << std::endl;
    if (server->listen() == PN_ERROR) {
        std::cerr << "Error: " << pw::universal_strerror() << std::endl;
        return 1;
    }

    pn::quit();
    return 0;
}
