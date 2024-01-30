#include "Polyweb/polyweb.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <openssl/sha.h>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>

const time_t running_since = time(nullptr);

std::string sockaddr_to_string(const struct sockaddr* addr) {
    std::string ret;
    switch (addr->sa_family) {
    case AF_INET: {
        struct sockaddr_in inet_addr;
        memcpy(&inet_addr, addr, sizeof inet_addr);
        pn::inet_ntop(AF_INET, &inet_addr.sin_addr, ret);
        ret += ':' + std::to_string(ntohs(inet_addr.sin_port));
        break;
    }

    case AF_INET6: {
        struct sockaddr_in6 inet6_addr;
        memcpy(&inet6_addr, addr, sizeof inet6_addr);
        pn::inet_ntop(AF_INET6, &inet6_addr.sin6_addr, ret);
        ret += ':' + std::to_string(ntohs(inet6_addr.sin6_port));
        break;
    }

    default:
        return "Unknown address family";
    }
    return ret;
}

struct ClientInfo {
    std::string current_job;
    time_t rawtime;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Error: Too few arguments" << std::endl;
        return 1;
    }
    std::string port = argc >= 4 ? argv[3] : "3000";

    std::ifstream prompts_file(argv[1]);
    std::ofstream responses_file(argv[2], std::ostream::app);
    if (!prompts_file.is_open() || !responses_file.is_open()) {
        std::cerr << "Error: Could not open file" << std::endl;
        return 1;
    }

    std::mutex mutex;
    std::unordered_map<std::string, size_t> contributors;
    std::map<std::string, size_t> activity;
    std::queue<std::string> queue;
    for (std::string line; std::getline(prompts_file, line); queue.push(std::move(line))) {}
    prompts_file.close();

    pn::init();
    pn::UniqueSocket<pw::Server> server;

#ifdef _WIN32
    DWORD timeout = 300000;
#else
    struct timeval timeout;
    timeout.tv_sec = 300;
    timeout.tv_usec = 0;
#endif
    if (server->setsockopt(SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) == PN_ERROR) {
        std::cerr << "Error: " << pn::universal_strerror() << std::endl;
        return 1;
    }
    if (server->setsockopt(SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == PN_ERROR) {
        std::cerr << "Error: " << pn::universal_strerror() << std::endl;
        return 1;
    }

    static constexpr int tcp_keep_alive = 1;
    if (server->setsockopt(SOL_SOCKET, SO_KEEPALIVE, &tcp_keep_alive, sizeof(int)) == PN_ERROR) {
        std::cerr << "Error: " << pn::universal_strerror() << std::endl;
        return 1;
    }

    server->route_ws(
        "/",
        pw::WSRoute {
            [&mutex, &contributors, &queue](pw::Connection& conn, void*) {
                ClientInfo* client_info = new ClientInfo;
                conn.data = client_info;

                std::unique_lock<std::mutex> lock(mutex);
                if (!queue.empty()) {
                    contributors[sockaddr_to_string(&conn.addr)] = 0;
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
            [&responses_file, &mutex, &contributors, &activity, &queue](pw::Connection& conn, pw::WSMessage message, void*) {
                if (message.opcode != 1 || message.data.empty()) {
                    conn.close();
                    return;
                }

                // Check proof of work
                unsigned char digest[SHA256_DIGEST_LENGTH];
                SHA256((const unsigned char*) message.data.data(), message.data.size(), digest);
                uint32_t first_24_bits = (((uint32_t) digest[0]) << 16) | (((uint32_t) digest[1]) << 8) | digest[2];
                if (first_24_bits && __builtin_clzl(first_24_bits << 8) < 20) {
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
                    ++contributors[sockaddr_to_string(&conn.addr)];

#ifdef _WIN32
                    struct tm timeinfo = *localtime(&rawtime);
#else
                    time_t rawtime = time(nullptr);
                    struct tm timeinfo;
                    localtime_r(&rawtime, &timeinfo);
#endif
                    std::ostringstream ss;
                    ss.imbue(std::locale("C"));
                    ss << std::put_time(&timeinfo, "%m/%d/%y");
                    decltype(activity)::iterator day_it;
                    if ((day_it = activity.find(ss.str())) != activity.end()) {
                        ++day_it->second;
                    } else {
                        if (activity.size() >= 180) {
                            activity.clear();
                        }
                        activity[ss.str()] = 1;
                    }

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
            [&mutex, &contributors, &queue](pw::Connection& conn, uint16_t status_code, const std::string& reason, bool clean, void*) {
                auto client_info = (ClientInfo*) conn.data;
                mutex.lock();
                queue.push(client_info->current_job);
                contributors.erase(sockaddr_to_string(&conn.addr));
                mutex.unlock();
                delete client_info;

                std::cout << "Client " << (clean ? "cleanly" : "uncleanly") << " disconnected from job server" << std::endl;
            },
        });

    server->route("/stats",
        pw::HTTPRoute {
            [&mutex, &contributors, &activity, &queue, initial_queue_size = queue.size()](const pw::Connection&, const pw::HTTPRequest& req, void*) {
                std::lock_guard<std::mutex> lock(mutex);
                std::ostringstream html;
                html.imbue(std::locale("en_US.UTF-8"));
                html << std::fixed << std::setprecision(3);
                html << "<html>";
                html << "<head>";
                html << "<title>Contribution Statistics</title>";
                html << "<style>html { margin: 0; padding: 0; } body { margin: 0; padding: 10px; font-family: sans-serif; color: rgb(204, 204, 204); background-color: rgb(17, 17, 17); } h1, h2, h3, h4, h5, h6 { color: #FFFFFF; } a { color: #4287F5; }</style>";
                html << "</head>";

                html << "<body style=\"display: flex; flex-direction: column; box-sizing: border-box; height: 100%;\">";
                html << "<h1 style=\"margin: 5px; text-align: center;\">Contribution Statistics</h1>";

                html << "<div style=\"display: flex; flex: 1; min-height: 0;\">";
                html << "<div style=\"flex: 1; min-width: 0; margin: 10px; overflow-y: auto;\"/>";
                html << "<p><strong>Running since:</strong> " << pw::build_date(running_since) << "</p>";
                html << "<p><strong>Queue size:</strong> " << queue.size() << '/' << initial_queue_size << "</p>";
                html << "<p><strong>Contributions per minute:</strong> " << (initial_queue_size - queue.size()) / ((time(nullptr) - running_since) / 60.f) << "</p>";

                html << "<p><strong>Unique contributors:</strong> " << contributors.size() << "</p>";
                if (!contributors.empty()) {
                    html << "<p><strong>Most active contributors:</strong></p>";
                    html << "<ol>";
                    std::vector<std::pair<std::string, size_t>> contributor_pairs(contributors.begin(), contributors.end());
                    std::sort(contributor_pairs.begin(), contributor_pairs.end(), [](const auto& a, const auto& b) {
                        return a.second > b.second;
                    });
                    for (const auto& contributor : contributor_pairs) {
                        html << "<li>" << pw::escape_xml(contributor.first) << " - " << contributor.second << " contribution(s)</li>";
                    }
                    html << "</ol>";
                }
                html << "</div>";

                html << "<div style=\"flex: 1; min-width: 0; margin: 10px; padding: 10px; background-color: rgb(34, 34, 34); border-radius: 10px;\"><canvas id=\"chart\"></canvas></div>";
                html << "</div>";

                html << "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>";
                html << "<script>";
                html << "const labels = [";
                for (const auto& day : activity) {
                    html << std::quoted(day.first) << ',';
                }
                html << "];";
                html << "const data = [";
                for (const auto& day : activity) {
                    html << std::to_string(day.second) << ',';
                }
                html << "];";
                html << R"delimiter(
                    const ctx = document.getElementById("chart");

                    Chart.defaults.color = "rgb(204, 204, 204)";
                    new Chart(ctx, {
                        type: "bar",
                        data: {
                            labels,
                            datasets: [{
                                label: "# of Contributions",
                                backgroundColor: "#0EA5E9",
                                data,
                                borderWidth: 1,
                            }],
                        },
                        options: {
                            maintainAspectRatio: false,
                            scales: {
                                x: {
                                    grid: {
                                        color: "rgb(85, 85, 85)",
                                    },
                                },
                                y: {
                                    beginAtZero: true,
                                    grid: {
                                        color: "rgb(85, 85, 85)",
                                    },
                                },
                            },
                        },
                    });
                )delimiter";
                html << "</script>";

                html << "</body>";
                html << "</html>";
                return pw::HTTPResponse(200, html.str(), {{"Content-Type", "text/html"}});
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
