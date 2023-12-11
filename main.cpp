#include "Polyweb/polyweb.hpp"
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>

struct ClientInfo {
    std::string current_job;
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

    server->route_ws("/",
        pw::WSRoute {
            [&mutex, &queue](pw::Connection& conn, void*) {
                ClientInfo* client_info = new ClientInfo;
                conn.data = client_info;
                mutex.lock();
                if (!queue.empty()) {
                    client_info->current_job = queue.front();
                    queue.pop();
                } else {
                    std::cout << "Finished!" << std::endl;
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

                auto client_info = (ClientInfo*) conn.data;
                std::string response = message.to_string();
                if (response != "SKIP_PROMPT") {
                    responses_file << client_info->current_job << '\t' << response << std::endl;
                }
                mutex.lock();
                if (!queue.empty()) {
                    client_info->current_job = queue.front();
                    queue.pop();
                } else {
                    std::cout << "Finished!" << std::endl;
                    exit(0);
                }
                mutex.unlock();
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
