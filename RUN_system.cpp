#include <iostream>
#include <limits>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <cctype>
#include <cstdlib>
#include <csignal>

#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "DataProcessor.h"
#include "GreedyScheduler.h"
#include "BacktrackingScheduler.h"
#include "DPScheduler.h"

#pragma comment(lib, "ws2_32")
#pragma comment(lib, "shell32")

using namespace std;
namespace fs = std::filesystem;

// ======================================================
// CHUYEN CHUOI THANH JSON STRING AN TOAN
// ======================================================

string escapeJSON(const string& s) {
string out;

for (unsigned char c : s) {
    switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;

        default:
            if (c < 0x20) {
                const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0x0F];
                out += hex[c & 0x0F];
            } else {
                out += static_cast<char>(c);
            }
    }
}

return out;

}

// ======================================================
// TAO HTTP RESPONSE
// ======================================================

string makeResponse(
const string& body,
const string& type = "application/json",
const string& status = "200 OK"
) {
stringstream ss;

ss << "HTTP/1.1 " << status << "\r\n"
   << "Content-Type: " << type << "; charset=utf-8\r\n"
   << "Access-Control-Allow-Origin: *\r\n"
   << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
   << "Access-Control-Allow-Headers: Content-Type\r\n"
   << "Connection: close\r\n"
   << "Content-Length: " << body.size() << "\r\n\r\n"
   << body;

return ss.str();

}

void sendText(SOCKET client, const string& response) {
size_t sent = 0;

while (sent < response.size()) {
    int chunkSize = static_cast<int>(
        min(response.size() - sent,
            static_cast<size_t>(numeric_limits<int>::max()))
    );

    int result = send(
        client,
        response.c_str() + sent,
        chunkSize,
        0
    );

    if (result == SOCKET_ERROR || result == 0) {
        break;
    }

    sent += result;
}

}

// ======================================================
// DOC HTTP REQUEST DAY DU
// ======================================================

string toLowerString(string value) {
transform(
value.begin(),
value.end(),
value.begin(),
[](unsigned char c) {
return static_cast<char>(tolower(c));
}
);

return value;

}

string getHeaderValue(const string& request, const string& headerName) {
size_t headerEnd = request.find("\r\n\r\n");

if (headerEnd == string::npos) {
    return "";
}

string headers = request.substr(0, headerEnd);
string target = toLowerString(headerName);

stringstream ss(headers);
string line;

getline(ss, line);

while (getline(ss, line)) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    size_t colon = line.find(':');

    if (colon == string::npos) {
        continue;
    }

    string name = toLowerString(line.substr(0, colon));

    if (name == target) {
        size_t valueStart = line.find_first_not_of(
            " \t",
            colon + 1
        );

        if (valueStart == string::npos) {
            return "";
        }

        return line.substr(valueStart);
    }
}

return "";

}

string readHttpRequest(SOCKET client) {
string request;
char buffer[8192];

// Doc phan dau request.
while (request.find("\r\n\r\n") == string::npos) {
    int received = recv(
        client,
        buffer,
        sizeof(buffer),
        0
    );

    if (received <= 0) {
        return "";
    }

    request.append(buffer, received);

    // Gioi han header de tranh request bat thuong.
    if (request.size() > 1024 * 1024) {
        return "";
    }
}

size_t headerEnd = request.find("\r\n\r\n");
size_t bodyStart = headerEnd + 4;

string lengthText = getHeaderValue(
    request,
    "Content-Length"
);

size_t contentLength = 0;

if (!lengthText.empty()) {
    try {
        contentLength = static_cast<size_t>(
            stoull(lengthText)
        );
    } catch (...) {
        return "";
    }
}

// Gioi han body 10 MB.
if (contentLength > 10 * 1024 * 1024) {
    return "";
}

while (request.size() - bodyStart < contentLength) {
    int received = recv(
        client,
        buffer,
        sizeof(buffer),
        0
    );

    if (received <= 0) {
        break;
    }

    request.append(buffer, received);
}

if (request.size() - bodyStart < contentLength) {
    return "";
}

return request;

}

string getRequestBody(const string& request) {
size_t bodyPos = request.find("\r\n\r\n");

if (bodyPos == string::npos) {
    return "";
}

return request.substr(bodyPos + 4);

}

struct AppData {
    vector<Course> courses;
    vector<Student> students;
    vector<ExamRoom> rooms;
    vector<TimeSlot> timeSlots;
    unordered_map<string, vector<Student>> courseStudentsMap;
};

bool loadAppData(AppData& data, const fs::path& inputDirectory) {
    DataProcessor dataProcessor;
    if (!dataProcessor.loadFromFiles(
            inputDirectory / "data_Config.txt",
            inputDirectory / "data_Courses.txt",
            inputDirectory / "data_Students.txt",
            inputDirectory / "data_Rooms.txt",
            inputDirectory / "data_Time_Slots.txt"
        )) {
        cerr << "Không thể tải dữ liệu!\n";
        return false;
    }

    if (!dataProcessor.validateData()) {
        cerr << "Dữ liệu không hợp lệ. Hãy kiểm tra lại!\n";
        return false;
    }

    data.courses = dataProcessor.getCourses();
    data.students = dataProcessor.getStudents();
    data.rooms = dataProcessor.getRooms();
    data.timeSlots = dataProcessor.getTimeSlots();
    data.courseStudentsMap.clear();
    data.courseStudentsMap.reserve(data.courses.size());

    for (const Student& student : data.students) {
        for (const string& courseId : student.getRegisteredCourses()) {
            data.courseStudentsMap[courseId].push_back(student);
        }
    }

    return true;
}

// ======================================================
// API DOC DU LIEU
// GET /api/data
// ======================================================

string getJSONData(const AppData& data) {
const auto& courses = data.courses;
const auto& students = data.students;
const auto& rooms = data.rooms;
const auto& slots = data.timeSlots;

unordered_map<string, int> courseStudentCount;

for (const auto& student : students) {
    for (const auto& courseId : student.getRegisteredCourses()) {
        courseStudentCount[courseId]++;
    }
}

ostringstream json;

json << "{\"ok\":true";
json << ",\"courses\":" << courses.size();
json << ",\"rooms\":" << rooms.size();
json << ",\"slots\":" << slots.size();
json << ",\"students\":" << students.size();
// DANH SACH DAY DU SINH VIEN

json << ",\"studentList\":[";

for (size_t i = 0; i < students.size(); ++i) {
    if (i > 0) json << ",";

    const auto& student = students[i];

    json << "{"
         << "\"studentId\":\""
         << escapeJSON(student.getStudentId()) << "\","
         << "\"fullName\":\""
         << escapeJSON(student.getFullName()) << "\","
         << "\"dateOfBirth\":\""
         << escapeJSON(student.getDateOfBirth()) << "\","
         << "\"gender\":\""
         << escapeJSON(student.getGender()) << "\","
         << "\"registeredCourses\":[";

    const auto& registeredCourses = student.getRegisteredCourses();
    for (size_t j = 0; j < registeredCourses.size(); ++j) {
        if (j > 0) json << ",";
        json << "\"" << escapeJSON(registeredCourses[j]) << "\"";
    }

    json << "]}";
}

json << "]";

// DANH SACH MON HOC

json << ",\"courseList\":[";

for (size_t i = 0; i < courses.size(); ++i) {
    if (i > 0) json << ",";

    const auto& course = courses[i];
    string courseId = course.getCourseId();

    json << "{"
         << "\"id\":\"" << escapeJSON(courseId) << "\","
         << "\"courseId\":\"" << escapeJSON(courseId) << "\","
         << "\"name\":\""
         << escapeJSON(course.getCourseName()) << "\","
         << "\"courseName\":\""
         << escapeJSON(course.getCourseName()) << "\","
         << "\"duration\":" << course.getDuration() << ","
         << "\"studentCount\":"
         << courseStudentCount[courseId]
         << "}";
}

json << "]";

// DANH SACH PHONG THI

json << ",\"roomList\":[";

for (size_t i = 0; i < rooms.size(); ++i) {
    if (i > 0) json << ",";

    const auto& room = rooms[i];

    json << "{"
         << "\"id\":\"" << escapeJSON(room.getRoomId()) << "\","
         << "\"roomId\":\"" << escapeJSON(room.getRoomId()) << "\","
         << "\"capacity\":" << room.getCapacity()
         << "}";
}

json << "]";

// DANH SACH CA THI

json << ",\"slotList\":[";

for (size_t i = 0; i < slots.size(); ++i) {
    if (i > 0) json << ",";

    const auto& slot = slots[i];

    json << "{"
         << "\"id\":\""
         << escapeJSON(slot.getTimeSlotId()) << "\","
         << "\"timeSlotId\":\""
         << escapeJSON(slot.getTimeSlotId()) << "\","
         << "\"date\":\""
         << escapeJSON(slot.getExamDate()) << "\","
         << "\"examDate\":\""
         << escapeJSON(slot.getExamDate()) << "\","
         << "\"start\":\""
         << escapeJSON(slot.getStartTime()) << "\","
         << "\"startTime\":\""
         << escapeJSON(slot.getStartTime()) << "\","
         << "\"end\":\""
         << escapeJSON(slot.getEndTime()) << "\","
         << "\"endTime\":\""
         << escapeJSON(slot.getEndTime()) << "\""
         << "}";
}

json << "]}";

return json.str();

}

// ======================================================
// CHAY THUAT TOAN VA TRA VE LICH THI
// POST /api/schedule
// ======================================================

vector<Schedule> runAlgorithm(
    const string& algorithm,
    const AppData& data,
    vector<string>& failedCourses
) {
    if (algorithm == "greedy") {
        return GreedyScheduler::schedule(
            data.courses,
            data.rooms,
            data.timeSlots,
            data.courseStudentsMap,
            failedCourses
        );
    }
    if (algorithm == "backtracking") {
        return BacktrackingScheduler::schedule(
            data.courses,
            data.rooms,
            data.timeSlots,
            data.courseStudentsMap,
            failedCourses
        );
    }
    return DPScheduler::runDPScheduler(
        data.courses,
        data.rooms,
        data.timeSlots,
        data.courseStudentsMap,
        failedCourses
    );
}

string runSchedule(const string& body, const AppData& data) {
string algorithm;

// Doc ten thuat toan tu JSON.
// Giu tuong thich voi frontend hien tai.

if (body.find("\"backtracking\"") != string::npos) {
    algorithm = "backtracking";
} else if (body.find("\"greedy\"") != string::npos) {
    algorithm = "greedy";
} else if (body.find("\"dp\"") != string::npos) {
    algorithm = "dp";
} else {
    return R"({"ok":false,"error":"Thuat toan khong hop le"})";
}

vector<string> failedCourses;
vector<Schedule> schedules;

auto startTime = chrono::steady_clock::now();

try {
    schedules = runAlgorithm(algorithm, data, failedCourses);
} catch (const exception& e) {
    return string("{\"ok\":false,\"error\":\"") +
        escapeJSON(e.what()) + "\"}";
}

auto endTime = chrono::steady_clock::now();

double elapsedMs =
    chrono::duration<double, milli>(
        endTime - startTime
    ).count();

string algorithmName =
    algorithm == "greedy" ? "Greedy" :
    algorithm == "backtracking" ? "Backtracking" :
    "Dynamic Programming";

// THONG KE THEO PHONG VA CA THI

struct Usage {
    int scheduleCount = 0;
    int studentCount = 0;
};

map<string, Usage> roomUsage;
map<string, Usage> slotUsage;

for (const auto& schedule : schedules) {
    string roomId = schedule.getRoom().getRoomId();
    string slotId = schedule.getTimeSlot().getTimeSlotId();

    int studentCount = static_cast<int>(
        schedule.getStudents().size()
    );

    roomUsage[roomId].scheduleCount++;
    roomUsage[roomId].studentCount += studentCount;

    slotUsage[slotId].scheduleCount++;
    slotUsage[slotId].studentCount += studentCount;
}

// DONG GOI JSON

ostringstream json;

json << "{\"ok\":true";
json << ",\"algorithm\":\"" << algorithmName << "\"";
json << ",\"runtimeMs\":" << elapsedMs;
json << ",\"total\":" << schedules.size();
json << ",\"failedCount\":" << failedCourses.size();

// DANH SACH LICH THI VA SINH VIEN

json << ",\"schedules\":[";

for (size_t i = 0; i < schedules.size(); ++i) {
    if (i > 0) json << ",";

    const auto& schedule = schedules[i];

    json << "{"
         << "\"id\":\""
         << escapeJSON(schedule.getScheduleId()) << "\","

         << "\"courseId\":\""
         << escapeJSON(
                schedule.getCourse().getCourseId()
            ) << "\","

         << "\"courseName\":\""
         << escapeJSON(
                schedule.getCourse().getCourseName()
            ) << "\","

         << "\"roomId\":\""
         << escapeJSON(
                schedule.getRoom().getRoomId()
            ) << "\","

         << "\"date\":\""
         << escapeJSON(
                schedule.getTimeSlot().getExamDate()
            ) << "\","

         << "\"start\":\""
         << escapeJSON(
                schedule.getTimeSlot().getStartTime()
            ) << "\","

         << "\"end\":\""
         << escapeJSON(
                schedule.getTimeSlot().getEndTime()
            ) << "\","

         << "\"studentCount\":"
         << schedule.getStudents().size()

         // DANH SACH SINH VIEN CUA LICH THI

         << ",\"students\":[";

    const auto& scheduleStudents = schedule.getStudents();

    for (size_t j = 0; j < scheduleStudents.size(); ++j) {
        if (j > 0) json << ",";

        const auto& student = scheduleStudents[j];

        json << "{"
             << "\"studentId\":\""
             << escapeJSON(student.getStudentId()) << "\","
             << "\"fullName\":\""
             << escapeJSON(student.getFullName()) << "\""
             << "}";
    }

    json << "]}";
}

json << "]";

// CAC MON CHUA XEP DUOC

json << ",\"failedCourses\":[";

for (size_t i = 0; i < failedCourses.size(); ++i) {
    if (i > 0) json << ",";

    json << "\""
         << escapeJSON(failedCourses[i])
         << "\"";
}

json << "]";

// THONG KE THEO PHONG

json << ",\"roomStatistics\":[";

bool first = true;

for (const auto& entry : roomUsage) {
    if (!first) json << ",";

    first = false;

    json << "{"
         << "\"id\":\"" << escapeJSON(entry.first) << "\","
         << "\"scheduleCount\":"
         << entry.second.scheduleCount << ","
         << "\"studentCount\":"
         << entry.second.studentCount
         << "}";
}

json << "]";

// THONG KE THEO CA THI

json << ",\"slotStatistics\":[";

first = true;

for (const auto& entry : slotUsage) {
    if (!first) json << ",";

    first = false;

    json << "{"
         << "\"id\":\"" << escapeJSON(entry.first) << "\","
         << "\"scheduleCount\":"
         << entry.second.scheduleCount << ","
         << "\"studentCount\":"
         << entry.second.studentCount
         << "}";
}

json << "]}";

return json.str();

}

// ======================================================
// MAIN SERVER
// ======================================================

void printResults(
    ostream& output,
    const string& algorithmName,
    const vector<Schedule>& schedules,
    const vector<string>& failedCourses,
    long long elapsedMicroseconds
) {
    output << "\n=== " << algorithmName << " ===\n";
    output << "Số môn đã xếp: " << schedules.size() << '\n';
    output << "Thời gian: " << elapsedMicroseconds << " micro giây\n";

    for (const Schedule& schedule : schedules) {
        const Course course = schedule.getCourse();
        const ExamRoom room = schedule.getRoom();
        const TimeSlot slot = schedule.getTimeSlot();

        output << schedule.getScheduleId()
               << " | " << course.getCourseId()
               << " - " << course.getCourseName()
               << " | " << slot.getExamDate()
               << " " << slot.getStartTime() << "-" << slot.getEndTime()
               << " | Phòng " << room.getRoomId()
               << " | " << schedule.getStudents().size() << " sinh viên"
               << '\n';
    }

    if (!failedCourses.empty()) {
        output << "Các môn chưa xếp được: ";
        for (size_t i = 0; i < failedCourses.size(); ++i) {
            if (i > 0) output << ", ";
            output << failedCourses[i];
        }
        output << '\n';
    }
}

bool runBatchAlgorithms(const AppData& data) {
    error_code outputError;
    fs::create_directories("output", outputError);
    if (outputError) {
        cerr << "Không thể tạo thư mục output: "
             << outputError.message() << '\n';
        return false;
    }

    ofstream outputFile(fs::path("output") / "output.txt");
    if (!outputFile) {
        cerr << "Không thể mở file output/output.txt để ghi.\n";
        return false;
    }
    outputFile.write("\xEF\xBB\xBF", 3);

    cout << "Dữ liệu hợp lệ. Bắt đầu chạy các thuật toán xếp lịch.\n";
    outputFile << "Dữ liệu hợp lệ. Bắt đầu chạy các thuật toán xếp lịch.\n";

    auto reportResults = [&](const string& algorithmName,
                             const vector<Schedule>& schedules,
                             const vector<string>& failedCourses,
                             long long elapsedMicroseconds) {
        printResults(cout, algorithmName, schedules, failedCourses, elapsedMicroseconds);
        printResults(outputFile, algorithmName, schedules, failedCourses, elapsedMicroseconds);
    };

    auto runAndReport = [&](const string& algorithm,
                            const string& algorithmName) {
        vector<string> failedCourses;
        const auto start = chrono::steady_clock::now();
        const vector<Schedule> schedules =
            runAlgorithm(algorithm, data, failedCourses);
        const auto elapsed = chrono::duration_cast<chrono::microseconds>(
            chrono::steady_clock::now() - start
        ).count();
        reportResults(algorithmName, schedules, failedCourses, elapsed);
    };

    runAndReport("greedy", "Thuật toán tham lam (Greedy)");
    runAndReport("dp", "Quy hoạch động (DP)");
    runAndReport("backtracking", "Quay lui (Backtracking)");

    outputFile.close();
    if (!outputFile) {
        cerr << "Không thể ghi đầy đủ file output/output.txt.\n";
        return false;
    }

    cout << "Đã ghi kết quả vào output/output.txt.\n";
    return true;
}

volatile std::sig_atomic_t shutdownRequested = 0;

void handleInterrupt(int) {
    shutdownRequested = 1;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
const HANDLE consoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
DWORD consoleMode;
if (consoleOutput != INVALID_HANDLE_VALUE &&
    GetConsoleMode(consoleOutput, &consoleMode) &&
    !SetConsoleOutputCP(CP_UTF8)) {
    cerr << "Khong the dat ma hoa console thanh UTF-8. Ma loi: "
         << GetLastError() << '\n';
    return 1;
}
#endif

if (argc > 2) {
    cerr << "Cách dùng: " << argv[0]
         << " [thu_muc_chua_du_lieu]\n";
    return 1;
}

const fs::path inputDirectory = argc == 2 ? argv[1] : "input";
AppData appData;
if (!loadAppData(appData, inputDirectory) || !runBatchAlgorithms(appData)) {
    return 1;
}

if (std::signal(SIGINT, handleInterrupt) == SIG_ERR) {
    cerr << "Khong the dang ky xu ly Ctrl+C.\n";
    return 1;
}

WSADATA wsa;

if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    cerr << "Khong khoi dong duoc Winsock.\n";
    return 1;
}

SOCKET serverSocket = socket(
    AF_INET,
    SOCK_STREAM,
    IPPROTO_TCP
);

if (serverSocket == INVALID_SOCKET) {
    cerr << "Khong tao duoc socket.\n";
    WSACleanup();
    return 1;
}

sockaddr_in serverAddress{};

serverAddress.sin_family = AF_INET;
serverAddress.sin_port = htons(8080);
serverAddress.sin_addr.s_addr = inet_addr("127.0.0.1");

if (bind(
        serverSocket,
        reinterpret_cast<sockaddr*>(&serverAddress),
        sizeof(serverAddress)
    ) == SOCKET_ERROR) {

    cerr << "Khong bind duoc cong 8080. "
         << "Co the cong dang duoc su dung.\n";

    closesocket(serverSocket);
    WSACleanup();
    return 1;
}

if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
    cerr << "Khong the lang nghe ket noi.\n";

    closesocket(serverSocket);
    WSACleanup();
    return 1;
}

cout << "Server dang chay tai http://127.0.0.1:8080\n";
cout << "Nhan Ctrl+C de dung.\n";
const HINSTANCE browserResult = ShellExecuteW(
    nullptr,
    L"open",
    L"http://127.0.0.1:8080/",
    nullptr,
    nullptr,
    SW_SHOWNORMAL
);
if (reinterpret_cast<INT_PTR>(browserResult) <= 32) {
    cerr << "Khong the tu mo trinh duyet. Hay mo http://127.0.0.1:8080/ thu cong.\n";
}

while (!shutdownRequested) {
    fd_set readableSockets;
    FD_ZERO(&readableSockets);
    FD_SET(serverSocket, &readableSockets);

    timeval timeout{};
    timeout.tv_usec = 250000;

    int ready = select(
        0,
        &readableSockets,
        nullptr,
        nullptr,
        &timeout
    );

    if (ready == 0) {
        continue;
    }

    if (ready == SOCKET_ERROR) {
        if (!shutdownRequested) {
            cerr << "Loi khi cho ket noi: "
                 << WSAGetLastError() << "\n";
        }
        break;
    }

    SOCKET client = accept(
        serverSocket,
        nullptr,
        nullptr
    );

    if (client == INVALID_SOCKET) {
        if (!shutdownRequested) {
            cerr << "Khong chap nhan duoc ket noi: "
                 << WSAGetLastError() << "\n";
        }
        continue;
    }

    string request = readHttpRequest(client);

    if (request.empty()) {
        closesocket(client);
        continue;
    }

    size_t lineEnd = request.find("\r\n");

    string firstLine = request.substr(0, lineEnd);

    string path = "/";

    size_t firstSpace = firstLine.find(' ');
    size_t secondSpace = firstLine.find(
        ' ',
        firstSpace == string::npos ? 0 : firstSpace + 1
    );

    if (firstSpace != string::npos &&
        secondSpace != string::npos) {

        path = firstLine.substr(
            firstSpace + 1,
            secondSpace - firstSpace - 1
        );
    }

    // Bo query string neu co.
    size_t queryPos = path.find('?');

    if (queryPos != string::npos) {
        path = path.substr(0, queryPos);
    }

    string response;

    // OPTIONS - CORS

    if (firstLine.find("OPTIONS ") == 0) {
        response = makeResponse("", "text/plain");
    }

    // API DU LIEU

    else if (path == "/api/data") {
        if (loadAppData(appData, inputDirectory)) {
            response = makeResponse(getJSONData(appData));
        } else {
            response = makeResponse(
                R"({"ok":false,"error":"Khong doc duoc du lieu"})",
                "application/json",
                "500 Internal Server Error"
            );
        }
    }

    // API XEP LICH

    else if (
        path == "/api/schedule" &&
        firstLine.find("POST ") == 0
    ) {
        string body = getRequestBody(request);

        if (loadAppData(appData, inputDirectory)) {
            response = makeResponse(runSchedule(body, appData));
        } else {
            response = makeResponse(
                R"({"ok":false,"error":"Khong doc duoc du lieu"})",
                "application/json",
                "500 Internal Server Error"
            );
        }
    }

    // GIAO DIEN WEB

    else {
        string filePath;

        if (path == "/" || path == "/index.html") {
            filePath = "web/index.html";
        } else if (path == "/script.js") {
            filePath = "web/script.js";
        } else if (path == "/style.css") {
            filePath = "web/style.css";
        }

        if (filePath.empty()) {
            response = makeResponse(
                R"({"ok":false,"error":"Not Found"})",
                "application/json",
                "404 Not Found"
            );
        } else {
            ifstream file(filePath, ios::binary);

            if (!file) {
                response = makeResponse(
                    R"({"ok":false,"error":"Khong tim thay file giao dien"})",
                    "application/json",
                    "404 Not Found"
                );
            } else {
                stringstream content;
                content << file.rdbuf();

                string type =
                    filePath.find(".css") != string::npos
                    ? "text/css" :
                    filePath.find(".js") != string::npos
                    ? "application/javascript" :
                    "text/html";

                response = makeResponse(
                    content.str(),
                    type
                );
            }
        }
    }

    sendText(client, response);

    shutdown(client, SD_SEND);
    closesocket(client);
}

cout << "\nServer da dung.\n";
closesocket(serverSocket);
WSACleanup();

return 0;

}


