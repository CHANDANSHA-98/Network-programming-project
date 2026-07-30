# 🚀 MiniHTTP Server v3

A lightweight, custom HTTP Web Server written in C for Windows using **WinSock2** socket programming. This project features static page delivery, an admin dashboard with authentication, backend REST API endpoints, real-time logging, and load-testing utilities.

---

## ✨ Features

* **WinSock Socket Architecture**: Pure C socket implementation built using Windows Winsock2 (`ws2_32`) and Process Status API (`psapi`).
* **Static File Web Server**: Serves HTML, CSS, and web utilities from the `www/` root directory.
* **Admin Dashboard & Authentication**: Includes secure routes with basic credential login (`admin` / `1234`).
* **Built-in System APIs**: Endpoints to stream dynamic system status, visitor count, and guestbook entries.
* **HTTP Logging**: Automated terminal logging with timestamps, HTTP methods, endpoints, and status codes.
* **Stress & Load Testing**: Dedicated multi-client test script (`client_loaadtest.c`) to test server concurrency and stability.

---

## 📁 Directory Structure

```text
npproject_updated/
│
├── www/                       # Static Web Assets Directory
│   ├── 404.html               # Custom 404 Error Page
│   ├── about.html             # About Page
│   ├── admin.html             # Admin Dashboard View
│   ├── calculator.html        # Interactive Calculator Tool
│   ├── guestbook.html         # Guestbook Web Interface
│   ├── index.html             # Main Homepage
│   ├── login.html             # Admin Login Form
│   ├── random.html            # Utility/Random Page
│   ├── status.html            # Server Status Dashboard
│   ├── style.css              # Main Stylesheet
│   └── time.html              # Dynamic Time Viewer
│
├── client.c                   # Basic C Test Client
├── client_loaadtest.c         # Concurrent Load Testing Utility
├── server.c                   # Core Web Server Implementation
└── README.md                  # Project Documentation
```

---

## 🛠️ Prerequisites

* **Operating System**: Windows OS
* **Compiler**: GCC / MinGW C Compiler
* **Libraries linked**: 
  * `ws2_32` (Windows Sockets)
  * `psapi` (Process Status API for system monitoring)

---

## ⚙️ Compilation & Setup

Open PowerShell or Command Prompt in your project directory (`npproject_updated`) and run the following compilation commands:

### Step 1: Compile the Web Server
```powershell
gcc server.c -o server.exe -lws2_32 -lpsapi
```

### Step 2: Compile the Test Client
```powershell
gcc client.c -o client.exe -lws2_32
```

### Step 3: Compile the Load Testing Utility
```powershell
gcc client_loaadtest.c -o client_loaadtest.exe -lws2_32
```

---

## 🚀 Execution & Running Steps

### Step 1: Launch the Web Server
In your main PowerShell terminal window, start the server executable:
```powershell
./server.exe
```

Upon launching, the server will initialize Winsock and start listening on port 8080:
```text
========================================
 MiniHTTP Server v3 running
 http://localhost:8080/
 Admin dashboard -> http://localhost:8080/admin
 Admin login -> user: admin  pass: 1234
========================================
```

### Step 2: Access via Web Browser
Open your browser (Chrome, Edge, Firefox, etc.) and navigate to:
* **Main Website**: [http://localhost:8080/](http://localhost:8080/)
* **Admin Login**: [http://localhost:8080/admin/login](http://localhost:8080/admin/login)
* **Admin Dashboard**: [http://localhost:8080/admin](http://localhost:8080/admin)

### Step 3: Run Test Clients (Optional)
Open a second PowerShell terminal window in the project folder and run either test script:

* **Basic Connection Test:**
  ```powershell
  ./client.exe
  ```

* **Concurrent Stress/Load Test:**
  ```powershell
  ./client_loaadtest.exe
  ```

---

## 🔐 Credentials & Admin Access

| Key | Value |
| :--- | :--- |
| **Admin Dashboard URL** | `http://localhost:8080/admin` |
| **Default Username** | `admin` |
| **Default Password** | `1234` |

---

## 🌐 Available Routes & API Endpoints

### Static Web Routes
* `GET /` or `/index.html` — Main Homepage
* `GET /about.html` — Project Overview
* `GET /time.html` — Live Server Clock
* `GET /calculator.html` — Server-side Calculator Interface
* `GET /guestbook.html` — Public Guestbook Form
* `GET /status.html` — Public Server Status
* `GET /admin` — Protected Admin Dashboard

### Admin Backend APIs
* `GET /admin/api/status` — JSON feed containing server uptime, active connections, and memory usage
* `GET /admin/api/guestbook` — JSON feed of recorded guestbook messages
* `GET /admin/api/visitors` — JSON feed tracking recent request history and client IP addresses
