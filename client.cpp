// P2P Client


#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <cstring>
#include <mutex>
#include <thread>
#include <fstream>
#include <algorithm>
#include <atomic>


#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
    typedef int socklen_t;
    #define CLOSE_SOCKET closesocket
    #include <sys/stat.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #define CLOSE_SOCKET close
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

using namespace std;

#define BUFFER_SIZE 65536
#define PIECE_SIZE 5120  // 5KB

// ==================== DATA STRUCTURES ====================

// Local file information for this peer
struct LocalFileInfo {
    string filepath;
    long file_size;
    int num_pieces;
    vector<bool> bit_vector;  // Which pieces this peer has (1 = has, 0 = doesn't have)
};

// peer_file_map[group_id][filename] = LocalFileInfo
map<string, map<string, LocalFileInfo>> peer_file_map;
mutex file_map_mutex;

// Global variables
string my_ip;
int my_port;
string tracker_ip;
int tracker_port;
atomic<bool> running(true);

// ==================== HELPER FUNCTIONS ====================

vector<string> split_string(const string& str, char delimiter) {
    vector<string> tokens;
    stringstream ss(str);
    string token;
    while (getline(ss, token, delimiter)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

long get_file_size(const string& filepath) {
    struct stat stat_buf;
    int rc = stat(filepath.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

int calculate_num_pieces(long file_size) {
    return (int)((file_size + PIECE_SIZE - 1) / PIECE_SIZE);
}

string get_filename(const string& filepath) {
    size_t pos = filepath.find_last_of("/\\");
    if (pos != string::npos) {
        return filepath.substr(pos + 1);
    }
    return filepath;
}

// ==================== NETWORK FUNCTIONS ====================

// Global persistent tracker connection
SOCKET tracker_socket = INVALID_SOCKET;
mutex tracker_mutex;

SOCKET connect_to_server(const string& ip, int port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());
    server_addr.sin_port = htons(port);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET;
    }
    
    return sock;
}

bool connect_to_tracker() {
    if (tracker_socket != INVALID_SOCKET) {
        return true; // Already connected
    }
    
    tracker_socket = connect_to_server(tracker_ip, tracker_port);
    if (tracker_socket == INVALID_SOCKET) {
        cerr << "ERROR: Cannot connect to tracker" << endl;
        return false;
    }
    
    cout << "[CLIENT] Connected to tracker" << endl;
    return true;
}

string send_to_tracker(const string& message) {
    lock_guard<mutex> lock(tracker_mutex);
    
    if (tracker_socket == INVALID_SOCKET) {
        if (!connect_to_tracker()) {
            return "ERROR: Cannot connect to tracker";
        }
    }
    
    // Send message
    int sent = send(tracker_socket, message.c_str(), (int)message.length(), 0);
    if (sent <= 0) {
        // Connection lost, try to reconnect
        CLOSE_SOCKET(tracker_socket);
        tracker_socket = INVALID_SOCKET;
        if (!connect_to_tracker()) {
            return "ERROR: Connection lost";
        }
        send(tracker_socket, message.c_str(), (int)message.length(), 0);
    }
    
    // Receive response
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int received = recv(tracker_socket, buffer, BUFFER_SIZE - 1, 0);
    
    if (received <= 0) {
        // Connection lost
        CLOSE_SOCKET(tracker_socket);
        tracker_socket = INVALID_SOCKET;
        return "ERROR: Connection lost";
    }
    
    return string(buffer);
}

// ==================== PIECE SELECTION ALGORITHM ====================

struct PeerInfo {
    string ip;
    int port;
    vector<bool> bit_vector;
    vector<int> assigned_pieces;
};

// Get bit vector from a peer
vector<bool> get_peer_bit_vector(const string& ip, int port, const string& group_id, const string& filename) {
    SOCKET sock = connect_to_server(ip, port);
    if (sock == INVALID_SOCKET) {
        return vector<bool>();
    }
    
    string request = "GET_BITVECTOR " + group_id + " " + filename;
    send(sock, request.c_str(), (int)request.length(), 0);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    recv(sock, buffer, BUFFER_SIZE - 1, 0);
    CLOSE_SOCKET(sock);
    
    string response(buffer);
    if (response.find("BITVECTOR:") == string::npos) {
        return vector<bool>();
    }
    
    // Parse bit vector
    vector<bool> bit_vec;
    size_t pos = response.find("BITVECTOR:") + 10;
    string bits = response.substr(pos);
    
    stringstream ss(bits);
    int bit;
    while (ss >> bit) {
        bit_vec.push_back(bit == 1);
    }
    
    return bit_vec;
}

// Round-robin piece assignment
void assign_pieces_round_robin(vector<PeerInfo>& peers, int num_pieces) {
    if (peers.empty()) return;
    
    int num_peers = (int)peers.size();
    
    for (int piece = 0; piece < num_pieces; piece++) {
        // Round-robin: try to assign to peer (piece % num_peers)
        // If that peer doesn't have it, try next peer
        for (int i = 0; i < num_peers; i++) {
            int peer_idx = (piece + i) % num_peers;
            
            // Check if this peer has the piece (piece must be within bit_vector bounds)
            if (piece < (int)peers[peer_idx].bit_vector.size() && 
                peers[peer_idx].bit_vector[piece]) {
                peers[peer_idx].assigned_pieces.push_back(piece);
                break;
            }
        }
    }
}

// ==================== DOWNLOAD FUNCTIONS ====================

struct DownloadTask {
    string peer_ip;
    int peer_port;
    string group_id;
    string filename;
    string dest_path;
    vector<int> pieces;
    long file_size;
};

void download_from_peer(DownloadTask task) {
    cout << "[DOWNLOAD] Connecting to peer " << task.peer_ip << ":" << task.peer_port << endl;
    
    SOCKET sock = connect_to_server(task.peer_ip, task.peer_port);
    if (sock == INVALID_SOCKET) {
        cerr << "[DOWNLOAD] Failed to connect to peer" << endl;
        return;
    }
    
    // Open destination file
    FILE* fp = fopen(task.dest_path.c_str(), "r+b");
    if (!fp) {
        // Create file if doesn't exist
        fp = fopen(task.dest_path.c_str(), "wb");
        if (!fp) {
            cerr << "[DOWNLOAD] Cannot open destination file" << endl;
            CLOSE_SOCKET(sock);
            return;
        }
        
        // Pre-allocate file
        fseek(fp, task.file_size - 1, SEEK_SET);
        fputc('\0', fp);
        fclose(fp);
        fp = fopen(task.dest_path.c_str(), "r+b");
    }
    
    // Download each assigned piece
    for (int piece : task.pieces) {
        string request = "GET_PIECE " + task.group_id + " " + task.filename + " " + to_string(piece);
        send(sock, request.c_str(), (int)request.length(), 0);
        
        // First receive the 4-byte size header
        uint32_t piece_size = 0;
        int header_received = 0;
        while (header_received < 4) {
            int r = recv(sock, ((char*)&piece_size) + header_received, 4 - header_received, 0);
            if (r <= 0) break;
            header_received += r;
        }
        
        if (piece_size == 0 || header_received < 4) {
            cerr << "[DOWNLOAD] Failed to receive piece " << piece << endl;
            continue;
        }
        
        // Now receive the piece data
        char buffer[PIECE_SIZE + 100];
        memset(buffer, 0, sizeof(buffer));
        
        int total_received = 0;
        while (total_received < (int)piece_size) {
            int bytes_received = recv(sock, buffer + total_received, (int)piece_size - total_received, 0);
            if (bytes_received <= 0) break;
            total_received += bytes_received;
        }
        
        if (total_received > 0) {
            // Write piece to correct position in file
            long offset = (long)piece * PIECE_SIZE;
            fseek(fp, offset, SEEK_SET);
            fwrite(buffer, 1, total_received, fp);
            
            cout << "[DOWNLOAD] Piece " << piece << " downloaded (" << total_received << " bytes)" << endl;
        }
    }
    
    fclose(fp);
    CLOSE_SOCKET(sock);
    
    cout << "[DOWNLOAD] Finished downloading from " << task.peer_ip << endl;
}

bool download_file(const string& group_id, const string& filename, const string& dest_path,
                   const vector<pair<string, int>>& peer_list, long file_size, int num_pieces) {
    
    cout << "[DOWNLOAD] Starting parallel download of " << filename << endl;
    cout << "[DOWNLOAD] File size: " << file_size << " bytes, Pieces: " << num_pieces << endl;
    cout << "[DOWNLOAD] Available peers: " << peer_list.size() << endl;
    
    // Get bit vectors from all peers
    vector<PeerInfo> peers;
    for (const auto& p : peer_list) {
        PeerInfo peer;
        peer.ip = p.first;
        peer.port = p.second;
        peer.bit_vector = get_peer_bit_vector(p.first, p.second, group_id, filename);
        
        if (!peer.bit_vector.empty()) {
            peers.push_back(peer);
            cout << "[DOWNLOAD] Got bit vector from " << p.first << ":" << p.second << endl;
        }
    }
    
    if (peers.empty()) {
        cerr << "[DOWNLOAD] No peers with valid bit vectors" << endl;
        return false;
    }
    
    // Assign pieces using round-robin
    assign_pieces_round_robin(peers, num_pieces);
    
    // Create download threads
    vector<thread> threads;
    
    for (auto& peer : peers) {
        if (peer.assigned_pieces.empty()) continue;
        
        DownloadTask task;
        task.peer_ip = peer.ip;
        task.peer_port = peer.port;
        task.group_id = group_id;
        task.filename = filename;
        task.dest_path = dest_path;
        task.pieces = peer.assigned_pieces;
        task.file_size = file_size;
        
        threads.emplace_back(download_from_peer, task);
    }
    
    // Wait for all downloads to complete
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    cout << "[DOWNLOAD] Download complete: " << dest_path << endl;
    
    return true;
}

// ==================== SERVER THREAD (Serve other peers) ====================

void handle_peer_request(SOCKET client_socket) {
    char buffer[BUFFER_SIZE];
    
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_received <= 0) break;
        
        string request(buffer);
        vector<string> args = split_string(request, ' ');
        
        if (args.empty()) continue;
        
        string cmd = args[0];
        
        if (cmd == "GET_BITVECTOR" && args.size() >= 3) {
            string group_id = args[1];
            string filename = args[2];
            
            lock_guard<mutex> lock(file_map_mutex);
            
            if (peer_file_map.find(group_id) != peer_file_map.end() &&
                peer_file_map[group_id].find(filename) != peer_file_map[group_id].end()) {
                
                LocalFileInfo& info = peer_file_map[group_id][filename];
                string response = "BITVECTOR:";
                
                for (bool bit : info.bit_vector) {
                    response += " " + to_string(bit ? 1 : 0);
                }
                
                send(client_socket, response.c_str(), (int)response.length(), 0);
            } else {
                const char* error_msg = "ERROR: File not found";
                send(client_socket, error_msg, (int)strlen(error_msg), 0);
            }
        }
        else if (cmd == "GET_PIECE" && args.size() >= 4) {
            string group_id = args[1];
            string filename = args[2];
            int piece_num = stoi(args[3]);
            
            lock_guard<mutex> lock(file_map_mutex);
            
            if (peer_file_map.find(group_id) != peer_file_map.end() &&
                peer_file_map[group_id].find(filename) != peer_file_map[group_id].end()) {
                
                LocalFileInfo& info = peer_file_map[group_id][filename];
                
                if (piece_num < (int)info.bit_vector.size() && info.bit_vector[piece_num]) {
                    // Read piece from file
                    FILE* fp = fopen(info.filepath.c_str(), "rb");
                    if (fp) {
                        long offset = (long)piece_num * PIECE_SIZE;
                        fseek(fp, offset, SEEK_SET);
                        
                        char piece_buffer[PIECE_SIZE];
                        size_t bytes_read = fread(piece_buffer, 1, PIECE_SIZE, fp);
                        fclose(fp);
                        
                        // Send size header (4 bytes) + piece data
                        uint32_t size = (uint32_t)bytes_read;
                        send(client_socket, (char*)&size, sizeof(size), 0);
                        send(client_socket, piece_buffer, (int)bytes_read, 0);
                    } else {
                        uint32_t size = 0;
                        send(client_socket, (char*)&size, sizeof(size), 0);
                    }
                } else {
                    uint32_t size = 0;
                    send(client_socket, (char*)&size, sizeof(size), 0);
                }
            } else {
                uint32_t size = 0;
                send(client_socket, (char*)&size, sizeof(size), 0);
            }
        }
    }
    
    CLOSE_SOCKET(client_socket);
}

void server_thread_func() {
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        cerr << "[SERVER] Socket creation failed" << endl;
        return;
    }
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(my_ip.c_str());
    server_addr.sin_port = htons(my_port);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cerr << "[SERVER] Bind failed on port " << my_port << endl;
        CLOSE_SOCKET(server_socket);
        return;
    }
    
    if (listen(server_socket, 10) == SOCKET_ERROR) {
        cerr << "[SERVER] Listen failed" << endl;
        CLOSE_SOCKET(server_socket);
        return;
    }
    
    cout << "[SERVER] Peer server listening on " << my_ip << ":" << my_port << endl;
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket == INVALID_SOCKET) {
            continue;
        }
        
        thread client_thread(handle_peer_request, client_socket);
        client_thread.detach();
    }
    
    CLOSE_SOCKET(server_socket);
}

// ==================== CLIENT THREAD (User commands) ====================

void print_help() {
    cout << "\n========== AVAILABLE COMMANDS ==========" << endl;
    cout << "create_user <user_id> <password>         - Register new user" << endl;
    cout << "login <user_id> <password>               - Login" << endl;
    cout << "logout                                   - Logout" << endl;
    cout << "create_group <group_id>                  - Create a new group" << endl;
    cout << "join_group <group_id>                    - Request to join group" << endl;
    cout << "leave_group <group_id>                   - Leave a group" << endl;
    cout << "list_groups                              - List all groups" << endl;
    cout << "list_requests <group_id>                 - List pending requests (owner)" << endl;
    cout << "accept_request <group_id> <user_id>      - Accept join request (owner)" << endl;
    cout << "upload_file <filepath> <group_id>        - Share file with group" << endl;
    cout << "list_files <group_id>                    - List files in group" << endl;
    cout << "download_file <group_id> <filename> <dest> - Download file" << endl;
    cout << "show_downloads                           - Show local files" << endl;
    cout << "help                                     - Show this help" << endl;
    cout << "quit                                     - Exit client" << endl;
    cout << "=========================================\n" << endl;
}

void client_thread_func() {
    string current_user = "";
    bool logged_in = false;
    
    print_help();
    
    while (running) {
        cout << (logged_in ? ("[" + current_user + "]> ") : "> ");
        
        string input;
        getline(cin, input);
        
        if (input.empty()) continue;
        
        vector<string> args = split_string(input, ' ');
        if (args.empty()) continue;
        
        string cmd = args[0];
        
        // Handle local commands
        if (cmd == "help") {
            print_help();
            continue;
        }
        else if (cmd == "quit") {
            if (logged_in) {
                send_to_tracker("logout");
            }
            running = false;
            break;
        }
        else if (cmd == "show_downloads") {
            lock_guard<mutex> lock(file_map_mutex);
            cout << "\n=== LOCAL FILES ===" << endl;
            for (const auto& group : peer_file_map) {
                cout << "Group: " << group.first << endl;
                for (const auto& file : group.second) {
                    cout << "  - " << file.first << " (" << file.second.file_size << " bytes)" << endl;
                }
            }
            cout << "==================\n" << endl;
            continue;
        }
        
        // Commands that need tracker communication
        string message = input;
        
        if (cmd == "login") {
            // Append our server port so tracker knows where we're listening
            message += " " + to_string(my_port);
        }
        else if (cmd == "upload_file" && args.size() >= 3) {
            string filepath = args[1];
            string group_id = args[2];
            
            // Check if file exists
            long file_size = get_file_size(filepath);
            if (file_size < 0) {
                cout << "ERROR: File not found: " << filepath << endl;
                continue;
            }
            
            int num_pieces = calculate_num_pieces(file_size);
            string filename = get_filename(filepath);
            
            // Store in local file map
            {
                lock_guard<mutex> lock(file_map_mutex);
                LocalFileInfo info;
                info.filepath = filepath;
                info.file_size = file_size;
                info.num_pieces = num_pieces;
                info.bit_vector.resize(num_pieces, true);  // We have all pieces
                peer_file_map[group_id][filename] = info;
            }
            
            // Send to tracker with file metadata
            message = "upload_file " + filepath + " " + group_id + " " + 
                      to_string(file_size) + " " + to_string(num_pieces);
        }
        else if (cmd == "download_file" && args.size() >= 4) {
            string group_id = args[1];
            string filename = args[2];
            string dest_path = args[3];
            
            // Ask tracker for peer list
            string response = send_to_tracker("download_file " + group_id + " " + filename);
            
            if (response.find("PEERS:") == string::npos) {
                cout << response << endl;
                continue;
            }
            
            // Parse response: "PEERS: ip1:port1 ip2:port2 ... SIZE:xyz PIECES:n"
            vector<pair<string, int>> peer_list;
            long file_size = 0;
            int num_pieces = 0;
            
            stringstream ss(response);
            string token;
            
            while (ss >> token) {
                if (token.find(':') != string::npos && token.find("SIZE") == string::npos && 
                    token.find("PIECES") == string::npos && token != "PEERS:") {
                    size_t colon = token.find(':');
                    string ip = token.substr(0, colon);
                    int port = stoi(token.substr(colon + 1));
                    peer_list.push_back({ip, port});
                }
                else if (token.find("SIZE:") == 0) {
                    file_size = stol(token.substr(5));
                }
                else if (token.find("PIECES:") == 0) {
                    num_pieces = stoi(token.substr(7));
                }
            }
            
            if (peer_list.empty()) {
                cout << "ERROR: No peers available" << endl;
                continue;
            }
            
            // Start parallel download
            bool success = download_file(group_id, filename, dest_path, peer_list, file_size, num_pieces);
            
            if (success) {
                // Update local file map
                {
                    lock_guard<mutex> lock(file_map_mutex);
                    LocalFileInfo info;
                    info.filepath = dest_path;
                    info.file_size = file_size;
                    info.num_pieces = num_pieces;
                    info.bit_vector.resize(num_pieces, true);
                    peer_file_map[group_id][filename] = info;
                }
                
                // Tell tracker we're now a seeder
                send_to_tracker("update_seeder " + group_id + " " + filename);
                
                cout << "SUCCESS: File downloaded to " << dest_path << endl;
            } else {
                cout << "ERROR: Download failed" << endl;
            }
            
            continue; 
        }
        
        string response = send_to_tracker(message);
        cout << response << endl;
        
        // Update local state based on response
        if (cmd == "login" && response.find("SUCCESS") != string::npos) {
            logged_in = true;
            current_user = args[1];
        }
        else if (cmd == "logout" && response.find("SUCCESS") != string::npos) {
            logged_in = false;
            current_user = "";
        }
    }
}

// ==================== MAIN ====================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cout << "Usage: client.exe <IP>:<PORT> <tracker_info_file>" << endl;
        cout << "Example: client.exe 127.0.0.1:6000 tracker_info.txt" << endl;
        return 1;
    }
    
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "ERROR: WSAStartup failed with error: " << result << endl;
        return 1;
    }
#endif
    
    // Parse client IP:PORT
    string client_addr = argv[1];
    size_t colon_pos = client_addr.find(':');
    if (colon_pos == string::npos) {
        cerr << "ERROR: Invalid format. Expected IP:PORT" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    my_ip = client_addr.substr(0, colon_pos);
    my_port = stoi(client_addr.substr(colon_pos + 1));
    
    // Read tracker address from file
    string tracker_file = argv[2];
    ifstream file(tracker_file);
    if (!file.is_open()) {
        cerr << "ERROR: Cannot open tracker info file" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    string tracker_addr;
    getline(file, tracker_addr);
    file.close();
    
    colon_pos = tracker_addr.find(':');
    if (colon_pos == string::npos) {
        cerr << "ERROR: Invalid tracker format" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    tracker_ip = tracker_addr.substr(0, colon_pos);
    tracker_port = stoi(tracker_addr.substr(colon_pos + 1));
    
    cout << "========================================" << endl;
    cout << "  P2P CLIENT (Windows)" << endl;
    cout << "  Client: " << my_ip << ":" << my_port << endl;
    cout << "  Tracker: " << tracker_ip << ":" << tracker_port << endl;
    cout << "========================================" << endl;
    
    // Start server thread (to serve other peers)
    thread server_thread(server_thread_func);
    
    // Run client thread (user commands)
    client_thread_func();
    
    // Cleanup
    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    
    // Close tracker connection
    if (tracker_socket != INVALID_SOCKET) {
        CLOSE_SOCKET(tracker_socket);
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    cout << "Goodbye!" << endl;
    return 0;
}
