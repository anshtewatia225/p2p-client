# P2P Group-Based File Sharing System

A peer-to-peer file sharing system inspired by BitTorrent, where users can share and download files from groups they belong to. Downloads happen in parallel from multiple peers using a round-robin piece selection algorithm.

## Features

- **Multi-threaded Tracker**: Centralized metadata server managing users, groups, and file information
- **Multi-threaded Client**: Each client has a server thread (to serve other peers) and a client thread (for user commands)
- **Parallel Downloads**: Download different pieces of a file from multiple peers simultaneously
- **Piece Selection Algorithm**: Round-robin distribution of pieces across available peers
- **Group-based Access Control**: Files are shared within groups; users must be group members to download

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         TRACKER                                 │
│  - User registration and authentication                        │
│  - Group management                                             │
│  - File metadata storage                                        │
│  - Peer discovery for downloads                                 │
└─────────────────────────────────────────────────────────────────┘
         ▲                    ▲                    ▲
         │                    │                    │
    ┌────┴────┐          ┌────┴────┐          ┌────┴────┐
    │ Client1 │◄────────►│ Client2 │◄────────►│ Client3 │
    │(Peer 1) │          │(Peer 2) │          │(Peer 3) │
    └─────────┘          └─────────┘          └─────────┘
       │ │                  │ │                  │ │
       │ └──────────────────┼─┴──────────────────┘ │
       └────────────────────┴──────────────────────┘
                    (Peer-to-Peer Download)
```

## Compilation

### Windows (MinGW/g++)

**Option 1: Using Make (if installed)**
```bash
make all
```

**Option 2: Manual compilation**
```bash
g++ -std=c++11 -o tracker.exe tracker.cpp -lws2_32
g++ -std=c++11 -o client.exe client.cpp -lws2_32
```

### Linux
```bash
make all
```

Or manually:
```bash
g++ -std=c++11 -pthread -o tracker tracker.cpp
g++ -std=c++11 -pthread -o client client.cpp
```

## Usage

### Windows (Quick Start)
Double-click these batch files in order:
1. `run_tracker.bat` - Starts the tracker server
2. `run_client1.bat` - Starts client 1 (port 6000)
3. `run_client2.bat` - Starts client 2 (port 6001)

> **Note:** To stop the tracker, press `Ctrl+C` or close the window.

### Manual Setup

#### 1. Create Tracker Info File
Create a file `tracker_info.txt` with the tracker's IP and port:
```
127.0.0.1:5000
```

#### 2. Start the Tracker
```bash
./tracker tracker_info.txt 1
```

#### 3. Start Clients
Start multiple clients on different ports:
```bash
# Terminal 1
./client 127.0.0.1:6000 tracker_info.txt

# Terminal 2
./client 127.0.0.1:6001 tracker_info.txt
```

## Commands

| Command | Description |
|---------|-------------|
| `create_user <user_id> <password>` | Register a new user |
| `login <user_id> <password>` | Login to the system |
| `logout` | Logout from the system |
| `create_group <group_id>` | Create a new group (you become owner) |
| `join_group <group_id>` | Request to join a group |
| `leave_group <group_id>` | Leave a group |
| `list_groups` | List all available groups |
| `list_requests <group_id>` | List pending join requests (owner only) |
| `accept_request <group_id> <user_id>` | Accept a join request (owner only) |
| `upload_file <filepath> <group_id>` | Share a file with a group |
| `list_files <group_id>` | List all files in a group |
| `download_file <group_id> <filename> <dest_filepath>` | Download a file (dest must include filename) |
| `show_downloads` | Show locally available files |
| `help` | Show help message |
| `quit` | Exit the client |

## Example Session

### Terminal 1 (User 1)
```bash
./client 127.0.0.1:6000 tracker_info.txt
> create_user alice password123
SUCCESS: User registered successfully
> login alice password123
SUCCESS: Login successful
[alice]> create_group mygroup
SUCCESS: Group created successfully
[alice]> upload_file /path/to/document.pdf mygroup
SUCCESS: File uploaded successfully
```

### Terminal 2 (User 2)
```bash
./client 127.0.0.1:6001 tracker_info.txt
> create_user bob password456
SUCCESS: User registered successfully
> login bob password456
SUCCESS: Login successful
[bob]> join_group mygroup
SUCCESS: Join request sent
```

### Terminal 1 (Accept request)
```bash
[alice]> list_requests mygroup
PENDING REQUESTS:
bob
[alice]> accept_request mygroup bob
SUCCESS: User added to group
```

### Terminal 2 (Download file)
```bash
[bob]> list_files mygroup
FILES:
document.pdf (1048576 bytes)
[bob]> download_file mygroup document.pdf ./downloads/document.pdf
[DOWNLOAD] Starting parallel download of document.pdf
[DOWNLOAD] File size: 1048576 bytes, Pieces: 205
[DOWNLOAD] Available peers: 1
[DOWNLOAD] Got bit vector from 127.0.0.1:6000
[DOWNLOAD] Piece 0 downloaded (5120 bytes)
...
[DOWNLOAD] Download complete: ./downloads/document.pdf
SUCCESS: File downloaded to ./downloads/document.pdf
```

## Technical Details

### Piece Size
Files are divided into 5KB (5120 bytes) pieces for transfer.

### Piece Selection Algorithm
Uses round-robin distribution:
1. Get bit vectors from all available peers
2. For each piece `i`, assign to peer `(i % num_peers)` if that peer has the piece
3. If not, try the next peer in round-robin fashion
4. Create parallel threads to download from each peer

### Data Structures

**Tracker:**
- `tracker_infomap`: Group ID → Group Info (owner, members, files, requests)
- `user_info`: User ID → User Info (password, IP, port, active status)
- `file_metadata`: Group ID → Filename → Metadata (size, num_pieces)
- `file_seeders`: Group ID → Filename → Set of User IDs

**Client:**
- `peer_file_map`: Group ID → Filename → Local File Info (path, size, bit_vector)
