// P2P Tracker Server


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


#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
    typedef int socklen_t;
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #define CLOSE_SOCKET close
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

using namespace std;

#define BUFFER_SIZE 65536
#define PIECE_SIZE 5120  // 5KB

// ==================== DATA STRUCTURES ====================

// tracker_infomap: stores group information
struct GroupInfo {
    string owner;
    vector<string> peers;           // Members of the group
    vector<string> files;           // Files shared in this group
    vector<string> pending_requests; // Join requests
};

// user_info: stores user information
struct UserInfo {
    string password;
    string ip;
    int port;
    bool is_active;
    map<string, vector<string>> group_files; // group_id -> list of files user has shared
};

// File metadata stored in tracker
struct FileMetadata {
    string filename;
    long file_size;
    int num_pieces;
    string sha256_hash;
};

// Global data structures with mutex protection
map<string, GroupInfo> tracker_infomap;
map<string, UserInfo> user_info;
map<string, map<string, FileMetadata>> file_metadata; // group_id -> filename -> metadata
map<string, map<string, set<string>>> file_seeders;   // group_id -> filename -> set of user_ids

mutex data_mutex;

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

string join_vector(const vector<string>& vec, const string& delimiter) {
    string result;
    for (size_t i = 0; i < vec.size(); i++) {
        result += vec[i];
        if (i < vec.size() - 1) result += delimiter;
    }
    return result;
}

// Find logged-in user by their IP and port
string find_user_by_address(const string& ip, int port) {
    lock_guard<mutex> lock(data_mutex);
    for (const auto& pair : user_info) {
        if (pair.second.is_active && pair.second.ip == ip && pair.second.port == port) {
            return pair.first;
        }
    }
    return "";
}

void send_response(SOCKET client_socket, const string& response) {
    send(client_socket, response.c_str(), (int)response.length(), 0);
}

// ==================== COMMAND HANDLERS ====================

string handle_create_user(const vector<string>& args, const string& client_ip, int client_port) {
    if (args.size() < 3) {
        return "ERROR: Usage: create_user <user_id> <password>";
    }
    
    string user_id = args[1];
    string password = args[2];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (user_info.find(user_id) != user_info.end()) {
        return "ERROR: User already exists";
    }
    
    UserInfo new_user;
    new_user.password = password;
    new_user.ip = "";
    new_user.port = 0;
    new_user.is_active = false;
    
    user_info[user_id] = new_user;
    
    return "SUCCESS: User registered successfully";
}

string handle_login(const vector<string>& args, const string& client_ip, int client_port) {
    if (args.size() < 3) {
        return "ERROR: Usage: login <user_id> <password>";
    }
    
    string user_id = args[1];
    string password = args[2];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (user_info.find(user_id) == user_info.end()) {
        return "ERROR: User does not exist";
    }
    
    if (user_info[user_id].password != password) {
        return "ERROR: Invalid password";
    }
    
    if (user_info[user_id].is_active) {
        return "ERROR: User already logged in";
    }
    
    user_info[user_id].is_active = true;
    user_info[user_id].ip = client_ip;
    user_info[user_id].port = client_port;
    
    return "SUCCESS: Login successful";
}

string handle_logout(const vector<string>& args, const string& user_id) {
    lock_guard<mutex> lock(data_mutex);
    
    if (user_info.find(user_id) == user_info.end()) {
        return "ERROR: User not found";
    }
    
    user_info[user_id].is_active = false;
    user_info[user_id].ip = "";
    user_info[user_id].port = 0;
    
    return "SUCCESS: Logged out successfully";
}

string handle_create_group(const vector<string>& args, const string& user_id) {
    if (args.size() < 2) {
        return "ERROR: Usage: create_group <group_id>";
    }
    
    string group_id = args[1];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) != tracker_infomap.end()) {
        return "ERROR: Group already exists";
    }
    
    GroupInfo new_group;
    new_group.owner = user_id;
    new_group.peers.push_back(user_id);
    
    tracker_infomap[group_id] = new_group;
    
    return "SUCCESS: Group created successfully";
}

string handle_join_group(const vector<string>& args, const string& user_id) {
    if (args.size() < 2) {
        return "ERROR: Usage: join_group <group_id>";
    }
    
    string group_id = args[1];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    GroupInfo& group = tracker_infomap[group_id];
    
    // Check if already a member
    if (find(group.peers.begin(), group.peers.end(), user_id) != group.peers.end()) {
        return "ERROR: Already a member of this group";
    }
    
    // Check if request already pending
    if (find(group.pending_requests.begin(), group.pending_requests.end(), user_id) 
        != group.pending_requests.end()) {
        return "ERROR: Join request already pending";
    }
    
    group.pending_requests.push_back(user_id);
    
    return "SUCCESS: Join request sent";
}

string handle_leave_group(const vector<string>& args, const string& user_id) {
    if (args.size() < 2) {
        return "ERROR: Usage: leave_group <group_id>";
    }
    
    string group_id = args[1];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    GroupInfo& group = tracker_infomap[group_id];
    
    // Check if member
    auto it = find(group.peers.begin(), group.peers.end(), user_id);
    if (it == group.peers.end()) {
        return "ERROR: Not a member of this group";
    }
    
    if (group.owner == user_id) {
        return "ERROR: Owner cannot leave the group. Transfer ownership first.";
    }
    
    // Remove user from group
    group.peers.erase(it);
    
    // Remove user's files from this group
    if (user_info[user_id].group_files.find(group_id) != user_info[user_id].group_files.end()) {
        for (const string& filename : user_info[user_id].group_files[group_id]) {
            if (file_seeders[group_id].find(filename) != file_seeders[group_id].end()) {
                file_seeders[group_id][filename].erase(user_id);
            }
        }
        user_info[user_id].group_files.erase(group_id);
    }
    
    return "SUCCESS: Left group successfully";
}

string handle_list_groups(const vector<string>& args, const string& user_id) {
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.empty()) {
        return "No groups available";
    }
    
    string result = "GROUPS:\n";
    for (const auto& pair : tracker_infomap) {
        result += pair.first + " (Owner: " + pair.second.owner + ", Members: " 
                  + to_string(pair.second.peers.size()) + ")\n";
    }
    
    return result;
}

string handle_list_requests(const vector<string>& args, const string& user_id) {
    if (args.size() < 2) {
        return "ERROR: Usage: list_requests <group_id>";
    }
    
    string group_id = args[1];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    if (tracker_infomap[group_id].owner != user_id) {
        return "ERROR: Only group owner can view requests";
    }
    
    const vector<string>& requests = tracker_infomap[group_id].pending_requests;
    
    if (requests.empty()) {
        return "No pending requests";
    }
    
    string result = "PENDING REQUESTS:\n";
    for (const string& req : requests) {
        result += req + "\n";
    }
    
    return result;
}

string handle_accept_request(const vector<string>& args, const string& user_id) {
    if (args.size() < 3) {
        return "ERROR: Usage: accept_request <group_id> <user_id>";
    }
    
    string group_id = args[1];
    string request_user = args[2];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    GroupInfo& group = tracker_infomap[group_id];
    
    if (group.owner != user_id) {
        return "ERROR: Only group owner can accept requests";
    }
    
    auto it = find(group.pending_requests.begin(), group.pending_requests.end(), request_user);
    if (it == group.pending_requests.end()) {
        return "ERROR: No pending request from this user";
    }
    
    // Remove from pending and add to members
    group.pending_requests.erase(it);
    group.peers.push_back(request_user);
    
    return "SUCCESS: User added to group";
}

string handle_upload_file(const vector<string>& args, const string& user_id) {
    if (args.size() < 5) {
        return "ERROR: Usage: upload_file <filepath> <group_id> <file_size> <num_pieces>";
    }
    
    string filepath = args[1];
    string group_id = args[2];
    long file_size = stol(args[3]);
    int num_pieces = stoi(args[4]);
    
    // Extract filename from path
    string filename = filepath;
    size_t pos = filepath.find_last_of("/\\");
    if (pos != string::npos) {
        filename = filepath.substr(pos + 1);
    }
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    GroupInfo& group = tracker_infomap[group_id];
    
    // Check if user is member
    if (find(group.peers.begin(), group.peers.end(), user_id) == group.peers.end()) {
        return "ERROR: Not a member of this group";
    }
    
    // Add file metadata
    FileMetadata meta;
    meta.filename = filename;
    meta.file_size = file_size;
    meta.num_pieces = num_pieces;
    
    file_metadata[group_id][filename] = meta;
    
    // Add to group files if not already there
    if (find(group.files.begin(), group.files.end(), filename) == group.files.end()) {
        group.files.push_back(filename);
    }
    
    // Add user as seeder
    file_seeders[group_id][filename].insert(user_id);
    
    // Track in user's files
    user_info[user_id].group_files[group_id].push_back(filename);
    
    return "SUCCESS: File uploaded successfully";
}

string handle_list_files(const vector<string>& args, const string& user_id) {
    if (args.size() < 2) {
        return "ERROR: Usage: list_files <group_id>";
    }
    
    string group_id = args[1];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    GroupInfo& group = tracker_infomap[group_id];
    
    // Check if user is member
    if (find(group.peers.begin(), group.peers.end(), user_id) == group.peers.end()) {
        return "ERROR: Not a member of this group";
    }
    
    if (group.files.empty()) {
        return "No files in this group";
    }
    
    string result = "FILES:\n";
    for (const string& file : group.files) {
        result += file;
        if (file_metadata[group_id].find(file) != file_metadata[group_id].end()) {
            result += " (" + to_string(file_metadata[group_id][file].file_size) + " bytes)";
        }
        result += "\n";
    }
    
    return result;
}

string handle_download_file(const vector<string>& args, const string& user_id) {
    if (args.size() < 3) {
        return "ERROR: Usage: download_file <group_id> <filename>";
    }
    
    string group_id = args[1];
    string filename = args[2];
    
    lock_guard<mutex> lock(data_mutex);
    
    if (!user_info[user_id].is_active) {
        return "ERROR: Please login first";
    }
    
    if (tracker_infomap.find(group_id) == tracker_infomap.end()) {
        return "ERROR: Group does not exist";
    }
    
    GroupInfo& group = tracker_infomap[group_id];
    
    // Check if user is member
    if (find(group.peers.begin(), group.peers.end(), user_id) == group.peers.end()) {
        return "ERROR: Not a member of this group";
    }
    
    // Check if file exists
    if (file_seeders[group_id].find(filename) == file_seeders[group_id].end()) {
        return "ERROR: File not found in group";
    }
    
    const set<string>& seeders = file_seeders[group_id][filename];
    
    // Build peer list with IP:PORT for active seeders
    string result = "PEERS:";
    bool found_active = false;
    
    for (const string& seeder : seeders) {
        if (seeder == user_id) continue; // Skip self
        
        if (user_info[seeder].is_active) {
            found_active = true;
            result += " " + user_info[seeder].ip + ":" + to_string(user_info[seeder].port);
        }
    }
    
    if (!found_active) {
        return "ERROR: No active seeders available";
    }
    
    // Add file metadata
    FileMetadata& meta = file_metadata[group_id][filename];
    result += " SIZE:" + to_string(meta.file_size);
    result += " PIECES:" + to_string(meta.num_pieces);
    
    return result;
}

string handle_update_seeder(const vector<string>& args, const string& user_id) {
    if (args.size() < 3) {
        return "ERROR: Usage: update_seeder <group_id> <filename>";
    }
    
    string group_id = args[1];
    string filename = args[2];
    
    lock_guard<mutex> lock(data_mutex);
    
    // Add user as seeder for this file
    file_seeders[group_id][filename].insert(user_id);
    user_info[user_id].group_files[group_id].push_back(filename);
    
    return "SUCCESS: Seeder updated";
}

// ==================== CLIENT HANDLER ====================

void handle_client(SOCKET client_socket) {
    char buffer[BUFFER_SIZE];
    string current_user = "";
    string client_ip = "";
    int client_port = 0;
    
    // Get client address
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_socket, (struct sockaddr*)&addr, &addr_len);
    client_ip = inet_ntoa(addr.sin_addr);
    
    cout << "[TRACKER] Client connected from " << client_ip << endl;
    
    while (true) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_received <= 0) {
            cout << "[TRACKER] Client disconnected" << endl;
            // Note: We don't logout on disconnect anymore - user stays active
            // They can reconnect with the same IP:port
            break;
        }
        
        string command(buffer);
        cout << "[TRACKER] Received: " << command << endl;
        
        vector<string> args = split_string(command, ' ');
        
        if (args.empty()) {
            send_response(client_socket, "ERROR: Empty command");
            continue;
        }
        
        string cmd = args[0];
        string response;
        
        // For login command, get the client port from the command
        if (cmd == "login" && args.size() >= 4) {
            client_port = stoi(args[3]);
        }
        
        // Look up current user by IP:port (for session persistence across connections)
        current_user = find_user_by_address(client_ip, client_port);
        
        // Handle commands
        if (cmd == "create_user") {
            response = handle_create_user(args, client_ip, client_port);
        }
        else if (cmd == "login") {
            // client_port already parsed above
            response = handle_login(args, client_ip, client_port);
            // current_user will be found by find_user_by_address on next command
        }
        else if (cmd == "logout") {
            response = handle_logout(args, current_user);
            if (response.find("SUCCESS") != string::npos) {
                current_user = "";
            }
        }
        else if (cmd == "create_group") {
            response = handle_create_group(args, current_user);
        }
        else if (cmd == "join_group") {
            response = handle_join_group(args, current_user);
        }
        else if (cmd == "leave_group") {
            response = handle_leave_group(args, current_user);
        }
        else if (cmd == "list_groups") {
            response = handle_list_groups(args, current_user);
        }
        else if (cmd == "list_requests") {
            response = handle_list_requests(args, current_user);
        }
        else if (cmd == "accept_request") {
            response = handle_accept_request(args, current_user);
        }
        else if (cmd == "upload_file") {
            response = handle_upload_file(args, current_user);
        }
        else if (cmd == "list_files") {
            response = handle_list_files(args, current_user);
        }
        else if (cmd == "download_file") {
            response = handle_download_file(args, current_user);
        }
        else if (cmd == "update_seeder") {
            response = handle_update_seeder(args, current_user);
        }
        else if (cmd == "quit") {
            if (!current_user.empty()) {
                handle_logout(args, current_user);
            }
            send_response(client_socket, "BYE");
            break;
        }
        else {
            response = "ERROR: Unknown command";
        }
        
        send_response(client_socket, response);
    }
    
    CLOSE_SOCKET(client_socket);
}

// ==================== MAIN ====================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cout << "Usage: tracker.exe <tracker_info_file> <tracker_no>" << endl;
        cout << "Example: tracker.exe tracker_info.txt 1" << endl;
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
    
    string tracker_file = argv[1];
    int tracker_no = stoi(argv[2]);
    
    // Read tracker IP:PORT from file
    ifstream file(tracker_file);
    if (!file.is_open()) {
        cerr << "ERROR: Cannot open tracker info file" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    string line;
    int line_no = 0;
    string tracker_addr;
    
    while (getline(file, line) && line_no < tracker_no) {
        tracker_addr = line;
        line_no++;
    }
    file.close();
    
    if (tracker_addr.empty()) {
        cerr << "ERROR: Tracker address not found in file" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    // Parse IP:PORT
    size_t colon_pos = tracker_addr.find(':');
    if (colon_pos == string::npos) {
        cerr << "ERROR: Invalid format. Expected IP:PORT" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    string ip = tracker_addr.substr(0, colon_pos);
    int port = stoi(tracker_addr.substr(colon_pos + 1));
    
    // Create socket
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        cerr << "ERROR: Socket creation failed" << endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    // Allow socket reuse
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        cerr << "ERROR: Bind failed" << endl;
        CLOSE_SOCKET(server_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    // Listen for connections
    if (listen(server_socket, 10) == SOCKET_ERROR) {
        cerr << "ERROR: Listen failed" << endl;
        CLOSE_SOCKET(server_socket);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    cout << "========================================" << endl;
    cout << "  P2P TRACKER SERVER (Windows)" << endl;
    cout << "  Listening on " << ip << ":" << port << endl;
    cout << "========================================" << endl;
    
    // Accept connections
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket == INVALID_SOCKET) {
            cerr << "ERROR: Accept failed" << endl;
            continue;
        }
        
        // Create thread to handle client using std::thread
        thread client_thread(handle_client, client_socket);
        client_thread.detach();
    }
    
    CLOSE_SOCKET(server_socket);
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    return 0;
}
