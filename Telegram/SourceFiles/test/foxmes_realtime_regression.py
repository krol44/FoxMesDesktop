import argparse
import pathlib
import re
import subprocess
import tempfile


parser = argparse.ArgumentParser()
parser.add_argument("--qt-prefix", required=True)
parser.add_argument("--bridge-source", type=pathlib.Path)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
backend = root / "Telegram/SourceFiles/custom_backend"
adapter = (backend / "native_delete_adapter.cpp").read_text()
request = adapter[adapter.index("namespace CustomBackend {"):adapter.index("bool NativeDeleteEffectAvailable()")]
bridge = (args.bridge_source or backend / "native_bridge.cpp").read_text()
start = bridge.index("void NativeBridge::handleEvent(const QJsonObject &event) {")
end = bridge.index('    if (type == u"message.created"_q', start)
dispatch = bridge[start:end]
transport = re.search(r"bool IsTransportFailure\(int status\) \{.*?^\}", (backend / "api_client.cpp").read_text(), re.S | re.M).group()

support = r'''
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <algorithm>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#define LOG(...)

QString operator""_q(const char16_t *text, std::size_t size) {
    return QString::fromUtf16(text, qsizetype(size));
}

template <typename P> class not_null {
public:
    not_null(P value) : _value(value) {}
    P get() const { return _value; }
private:
    P _value;
};

namespace gsl {
template <typename F> struct final_action {
    F callback;
    ~final_action() { callback(); }
};
template <typename F> final_action<F> finally(F callback) {
    return { std::move(callback) };
}
}

void Require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TestTimer {
    struct Pending {
        QPointer<QObject> context;
        std::function<void()> callback;
    };
    static inline std::deque<Pending> pending;
    static inline std::vector<int> delays;
    template <typename F> static void singleShot(int delay, QObject *context, F callback) {
        delays.push_back(delay);
        pending.push_back({ context, std::move(callback) });
    }
    static void fire() {
        Require(!pending.empty(), "retry timer missing");
        auto timer = std::move(pending.front());
        pending.pop_front();
        if (timer.context) {
            timer.callback();
        }
    }
    static void reset() {
        pending.clear();
        delays.clear();
    }
};

namespace CustomBackend {
class ApiClient final : public QObject {
public:
    using Callback = std::function<void(QJsonDocument, QString, int)>;
    struct Request {
        bool lookup;
        qint64 chatId;
        QList<qint64> ids;
        QString operationId;
        Callback done;
    };
    std::deque<Request> pending;
    std::vector<Request> sent;
    qint64 sequence = 0;
    void deleteMessages(qint64 chatId, const QList<qint64> &ids, const QString &operationId, Callback done) {
        pending.push_back({ false, chatId, ids, operationId, std::move(done) });
        sent.push_back({ false, chatId, ids, operationId, {} });
    }
    void operationResult(const QString &operationId, Callback done) {
        pending.push_back({ true, 0, {}, operationId, std::move(done) });
        sent.push_back({ true, 0, {}, operationId, {} });
    }
    void reply(QJsonDocument doc, int status) {
        Require(!pending.empty(), "request missing");
        auto request = std::move(pending.front());
        pending.pop_front();
        request.done(std::move(doc), status >= 200 && status < 300 ? QString() : u"network error"_q, status);
    }
    void setEventSequence(qint64 value) { sequence = value; }
};
}

#define QTimer TestTimer
'''

cursor = r'''
namespace CustomBackend {
namespace Conferences {
bool HandleEvent(void *, const QString &, const QJsonObject &) { return false; }
}
namespace Calls {
bool HandleEvent(void *, const QString &, const QJsonObject &) { return false; }
}
namespace ChatRequests {
bool HandleEvent(void *, const QString &type, const QJsonObject &) { return type == u"chat.request"_q; }
}
void RememberEventSequence(void *, qint64) {}

class NativeBridge {
public:
    qint64 _eventSeq = 100;
    qint64 _lastGapResumeFrom = 0;
    void *_session = nullptr;
    ApiClient api;
    int restarts = 0;
    int resyncs = 0;
    std::set<int> messages = {5, 6, 7};
    ApiClient &client() { return api; }
    void forceLiveUpdatesRestart() { ++restarts; }
    void resyncAfterGap(qint64 resume, qint64 observed) {
        ++resyncs;
        _eventSeq = (resume > 0 ? resume : observed) - 1;
    }
    void handleEvent(const QJsonObject &event);
};
'''

tests = r'''
void Deliver(NativeBridge &bridge, qint64 sequence, const QString &type, int id = 0) {
    bridge.handleEvent(QJsonObject{
        { "seq", sequence },
        { "type", type },
        { "data", QJsonObject{ { "id", id } } },
    });
}

void TestReplay() {
    auto bridge = NativeBridge();
    Deliver(bridge, 103, u"message.deleted"_q, 7);
    Require(bridge.restarts == 1 && bridge.resyncs == 0 && bridge._eventSeq == 100,
        "ordinary event gap skipped the missing deletions");
    Deliver(bridge, 101, u"message.deleted"_q, 5);
    Deliver(bridge, 102, u"message.deleted"_q, 6);
    Deliver(bridge, 103, u"message.deleted"_q, 7);
    Require(bridge.messages.empty() && bridge._eventSeq == 103, "replay did not apply every deletion");
    Deliver(bridge, 104, u"chat.request"_q);
    Require(bridge._eventSeq == 104 && bridge.api.sequence == 104, "adapter event did not advance the replay cursor");
    Deliver(bridge, 105, u"read.updated"_q);
    Deliver(bridge, 0, u"chat.typing"_q);
    Deliver(bridge, 103, u"message.deleted"_q, 5);
    Require(bridge._eventSeq == 105 && bridge.restarts == 1, "ephemeral or duplicate event changed the cursor");
    bridge.handleEvent(QJsonObject{
        { "seq", 120 }, { "type", u"gap.detected"_q },
        { "data", QJsonObject{ { "next_seq", 120 } } },
    });
    Require(bridge.resyncs == 1 && bridge._eventSeq == 119, "retention gap did not use its explicit recovery point");
    Deliver(bridge, 120, u"message.deleted"_q, 5);
    Require(bridge._eventSeq == 120, "replay after retention gap was discarded");
}

QJsonDocument Success() {
    return QJsonDocument(QJsonObject{ { "deleted_ids", QJsonArray{ 5, 6 } } });
}

QJsonDocument Done() {
    return QJsonDocument(QJsonObject{ { "status", u"done"_q }, { "result", Success().object() } });
}

void Start(ApiClient &client, int &completed, bool &success) {
    DeleteMessagesWithRecovery(&client, 42, { 5, 6 }, u"original-operation"_q,
        [&](QJsonDocument result) {
            ++completed;
            success = result.object().value("deleted_ids").isArray();
        });
}

void TestDeleteRecovery() {
    {
        TestTimer::reset();
        auto client = ApiClient();
        auto completed = 0;
        auto success = false;
        Start(client, completed, success);
        client.reply({}, 0);
        TestTimer::fire();
        Require(client.pending.front().lookup, "ambiguous delete was repeated before checking its operation");
        client.reply({}, 503);
        TestTimer::fire();
        client.reply(Done(), 200);
        Require(completed == 1 && success, "lost response was not recovered from the operation journal");
        Require(client.sent.size() == 3 && !client.sent[0].lookup && client.sent[1].lookup && client.sent[2].lookup,
            "committed delete was repeated after acknowledgement loss");
    }
    {
        TestTimer::reset();
        auto client = ApiClient();
        auto completed = 0;
        auto success = false;
        Start(client, completed, success);
        client.reply({}, 500);
        TestTimer::fire();
        client.reply(QJsonDocument(QJsonObject{ { "status", u"unknown"_q } }), 200);
        const auto &retry = client.pending.front();
        Require(!retry.lookup && retry.operationId == u"original-operation"_q && retry.chatId == 42 && retry.ids == QList<qint64>{5, 6},
            "retry changed the deletion operation or its payload");
        client.reply(Success(), 200);
        Require(completed == 1 && success, "uncommitted delete was not retried");
    }
    {
        TestTimer::reset();
        auto client = ApiClient();
        auto completed = 0;
        auto success = false;
        Start(client, completed, success);
        client.reply({}, 429);
        for (auto i = 0; i != 7; ++i) {
            TestTimer::fire();
            client.reply({}, 500);
        }
        Require(completed == 0 && TestTimer::delays == std::vector<int>{1000, 2000, 4000, 8000, 16000, 16000, 16000, 16000},
            "network failure stopped recovery or bypassed retry backoff");
        TestTimer::fire();
        client.reply(Done(), 200);
        Require(completed == 1 && success, "recovery did not finish after connection restoration");
    }
    {
        TestTimer::reset();
        auto client = ApiClient();
        auto completed = 0;
        auto success = true;
        Start(client, completed, success);
        client.reply({}, 403);
        Require(completed == 1 && !success && TestTimer::pending.empty(), "definitive rejection was retried or treated as success");
    }
    {
        TestTimer::reset();
        auto client = ApiClient();
        auto completed = 0;
        auto success = false;
        Start(client, completed, success);
        client.reply(QJsonDocument(QJsonObject{ { "ok", true } }), 200);
        Require(completed == 0, "incomplete deletion response was accepted");
        TestTimer::fire();
        client.reply(Done(), 200);
        Require(completed == 1 && success, "incomplete response did not resolve through the journal");
    }
    {
        TestTimer::reset();
        auto client = std::make_unique<ApiClient>();
        auto completed = 0;
        auto success = false;
        Start(*client, completed, success);
        client->reply({}, 0);
        client.reset();
        TestTimer::fire();
        Require(completed == 0, "retry outlived its client");
    }
    TestTimer::reset();
}
}

int main(int argc, char **argv) {
    auto application = QCoreApplication(argc, argv);
    try {
        CustomBackend::TestReplay();
        CustomBackend::TestDeleteRecovery();
        std::cout << "PASS: gap replay, adapter cursor, duplicate/ephemeral events, operation lookup, identical retry, backoff, rejection, malformed response, client lifetime\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''

source = support + "namespace CustomBackend {\n" + transport + "\n}\n" + request + "\n}\n" + cursor + dispatch
source += '''    if (type == u"message.deleted"_q) {
        messages.erase(data.value("id").toInt());
    }
}
'''
source += tests

with tempfile.TemporaryDirectory(prefix="foxmes-realtime-") as directory:
    folder = pathlib.Path(directory)
    (folder / "realtime.cpp").write_text(source)
    (folder / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.21)
project(FoxMesRealtimeRegression LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(foxmes_realtime realtime.cpp)
target_link_libraries(foxmes_realtime PRIVATE Qt6::Core)
''')
    for command in [
        ["cmake", "-S", directory, "-B", str(folder / "compiled"), "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_PREFIX_PATH=" + args.qt_prefix],
        ["cmake", "--build", str(folder / "compiled"), "--config", "Release"],
        [str(folder / "compiled/foxmes_realtime")],
    ]:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            print(result.stdout)
            raise SystemExit(result.returncode)
    print(result.stdout, end="")
