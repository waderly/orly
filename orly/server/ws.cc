/* <orly/server/ws.cc>

   Implements <orly/server/ws.h>.

   Copyright 2010-2014 OrlyAtomics, Inc.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include <orly/server/ws.h>

#include <cassert>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <base/as_str.h>
#include <base/fd.h>
#include <base/json.h>
#include <base/tmp_copy_to_file.h>
#include <base/tmp_dir_maker.h>
#include <orly/compiler.h>
#include <orly/error.h>
#include <orly/client/program/parse_stmt.h>
#include <orly/client/program/translate_expr.h>
#include <orly/indy/key.h>
#include <orly/sabot/state_dumper.h>
#include <orly/sabot/type_dumper.h>
#include <orly/type/orlyify.h>
#include <orly/var/jsonify.h>
#include <orly/var/sabot_to_var.h>

using namespace std;
using namespace std::placeholders;

using namespace Base;
using namespace Util;

using namespace Orly;
using namespace Orly::Client::Program;
using namespace Orly::Sabot;
using namespace Orly::Server;

/* The implementation of the TWs interface declared in the header. */
class TWsImpl final
    : public TWs {
  public:

  /* Starts up the server. */
  TWsImpl(
      TSessionManager *session_mngr, size_t thread_count,
      in_port_t port_number)
      : SessionManager(session_mngr),
        TmpDirMaker(MakePath({ P_tmpdir, "orly_ws_compile" }, {})),
        BgThreads(thread_count ? thread_count : 1) {
    assert(session_mngr);
    syslog(LOG_INFO, "ws compile tmp dir = \"%s\"", TmpDirMaker.GetPath().c_str());
    WsServer.clear_access_channels(websocketpp::log::alevel::all);
    WsServer.clear_error_channels(websocketpp::log::elevel::all);
    WsServer.init_asio();
    WsServer.set_reuse_addr(true);
    WsServer.set_open_handler(bind(&TWsImpl::OnOpen, this, _1));
    WsServer.set_close_handler(bind(&TWsImpl::OnClose, this, _1));
    WsServer.set_message_handler(bind(&TWsImpl::OnMsg, this, _1, _2));
    WsServer.set_socket_init_handler(
          [] (websocketpp::connection_hdl /*hdl*/, boost::asio::ip::tcp::socket &s) {
            boost::asio::ip::tcp::no_delay option(true);
            s.set_option(option);
          }
    );
    WsServer.set_listen_backlog(8192);
    WsServer.listen(port_number);
    WsServer.start_accept();
    try {
      for (auto &bg_thread: BgThreads) {
        bg_thread = thread(&TWsServer::run, &WsServer);
      }
    } catch (...) {
      this->~TWsImpl();
      throw;
    }
  }

  /* Shuts down the server. */
  virtual ~TWsImpl() {
    assert(this);
    WsServer.stop();
    for (auto &bg_thread: BgThreads) {
      if (bg_thread.joinable()) {
        bg_thread.join();
      }
    }
    Conns.clear();
  }

  private:

  /* We use ASIO for our socket I/O. */
  using TAsioConfig = websocketpp::config::asio;

  /* This somewhat awkward structure contains the definitions used by the
     websocket lirbary.  It differs from TAsioConfig only in that it turns
     the multithreading protection on.  Everything else is just copied in
     from the standard ASIO config.  This is fragile but follows the
     example given by the library. */
  struct TServerConfig : TAsioConfig {

    /* Copy in default types. */
    using concurrency_type          = TAsioConfig::concurrency_type;
    using request_type              = TAsioConfig::request_type;
    using response_type             = TAsioConfig::response_type;
    using message_type              = TAsioConfig::message_type;
    using con_msg_manager_type      = TAsioConfig::con_msg_manager_type;
    using endpoint_msg_manager_type = TAsioConfig::endpoint_msg_manager_type;

    /* Copy in more default types. */
    using alog_type     = TAsioConfig::alog_type;
    using elog_type     = TAsioConfig::elog_type;
    using rng_type      = TAsioConfig::rng_type;
    using endpoint_base = TAsioConfig::endpoint_base;

    /* Do acquire and release mutexes within the server. */
    static bool const enable_multithreading = true;

    /* As TServerConfig, but for the transport layer. */
    struct TTransportConfig : TAsioConfig::transport_config {

      /* Copy in yet more default types. */
      using concurrency_type = TAsioConfig::concurrency_type;
      using elog_type        = TAsioConfig::elog_type;
      using alog_type        = TAsioConfig::alog_type;
      using request_type     = TAsioConfig::request_type;
      using response_type    = TAsioConfig::response_type;

      /* Do acquire and release mutexes within the transport layer. */
      static bool const enable_multithreading = true;

      /* Default log levels. */
      static const websocketpp::log::level elog_level = websocketpp::log::elevel::none;
      static const websocketpp::log::level alog_level = websocketpp::log::alevel::none;

    };  // TWsImpl::TServerConfig::TTransportConfig

    /* Our configured transport type. */
    using transport_type = websocketpp::transport::asio::endpoint<TTransportConfig>;

  };  // TWsImpl::TServerConfig

  /* Pull in some types from the websocket ASIO implementation. */
  using TWsServer = websocketpp::server<TServerConfig>;
  using TMsgPtr = TWsServer::message_ptr;
  using TConnHndl = websocketpp::connection_hdl;

  /* Constructed by TWsImpl::OnOpen(), destroyed by TWsImpl::OnClose(). */
  class TConn final {
    NO_COPY(TConn);
    public:

    /* Cache the args. */
    TConn(TWsImpl *ws, TConnHndl hndl)
        : Ws(ws), Hndl(hndl), Exiting(false) {}

    /* True iff. we have processed an exit statement and so want the TCP
       connection to close. */
    bool IsExiting() const noexcept {
      assert(this);
      return Exiting;
    }

    /* Called by TWsImpl::OnMsg(). Parses and interprets a statement sent
       to us as a text message. */
    TJson OnMsg(TMsgPtr msg) {
      assert(this);
      assert(msg);
      TJson ret;
      ParseStmtStr(
          msg->get_payload().c_str(),
          [this, &ret](const TStmt *stmt) {
             stmt->Accept(TStmtVisitor(this, ret));
          }
      );
      return ret;
    }

    private:

    /* Interpret a statement. */
    class TStmtVisitor final
        : public TStmt::TVisitor {
      public:

      /* Cache the args. */
      TStmtVisitor(TConn *conn, TJson &result)
          : Conn(conn), Result(result) {}

      /* Echo. */
      virtual void operator()(const TEchoStmt *stmt) const override {
        assert(this);
        assert(stmt);
        void *alloc = alloca(SabotStateSize);
        //NOTE: That we parse apart the json is very fugly here.
        //TODO: Push TJson down into Var::Jsonify
        Result = TJson::Parse(AsStrFunc(&Var::Jsonify, Var::ToVar(*TWrapper(NewStateSabot(stmt->GetExpr(), alloc)))));
      }

      /* Exit. */
      virtual void operator()(const TExitStmt *) const override {
        assert(this);
        Conn->Exiting = true;
      }

      /* New session. */
      virtual void operator()(const TNewSessionStmt *) const override {
        assert(this);
        if (Conn->Session) {
          throw invalid_argument("session already established");
        }
        Conn->Session.reset(Conn->Ws->SessionManager->NewSession());
        Result = AsStr(Conn->Session->GetId());
      }

      /* Resume session. */
      virtual void operator()(const TResumeSessionStmt *stmt) const override {
        assert(this);
        assert(stmt);
        if (Conn->Session) {
          throw invalid_argument("session already established");
        }
        Conn->Session.reset(Conn->Ws->SessionManager->ResumeSession(Translate(stmt->GetIdExpr())));
        Result = AsStr(Conn->Session->GetId());
      }

      /* Set user id. */
      virtual void operator()(const TSetUserIdStmt *stmt) const override {
        assert(this);
        assert(stmt);
        TUuid user_id = Translate(stmt->GetIdExpr());
        GetSession()->SetUserId(user_id);
      }

      /* Set time-to-live. */
      virtual void operator()(const TSetTtlStmt *stmt) const override {
        assert(this);
        assert(stmt);
        TUuid durable_id = Translate(stmt->GetIdExpr());
        chrono::seconds ttl(stmt->GetIntExpr()->GetLexeme().AsInt());
        GetSession()->SetTtl(durable_id, ttl);
      }

      /* Install package. */
      virtual void operator()(const TInstallStmt *stmt) const override {
        assert(this);
        assert(stmt);
        vector<string> package_name;
        uint64_t version;
        TranslatePackage(package_name, version, stmt->GetPackageName());
        GetSession()->InstallPackage(package_name, version);
      }

      /* Uninstall package. */
      virtual void operator()(const TUninstallStmt *stmt) const override {
        assert(this);
        assert(stmt);
        vector<string> package_name;
        uint64_t version;
        TranslatePackage(package_name, version, stmt->GetPackageName());
        GetSession()->UninstallPackage(package_name, version);
      }

      /* New pov. */
      virtual void operator()(const TPovConsStmt *stmt) const override {
        assert(this);
        assert(stmt);
        bool is_safe = dynamic_cast<const TSafeGuarantee *>(stmt->GetPovGuarantee()) != nullptr;
        bool is_shared = dynamic_cast<const TSharedKind *>(stmt->GetPovKind()) != nullptr;
        TOpt<TUuid> parent_id;
        auto parent = dynamic_cast<const TParent *>(stmt->GetOptParent());
        if (parent) {
          parent_id = Translate(parent->GetIdExpr());
        }
        Result = AsStr(GetSession()->NewPov(is_safe, is_shared, parent_id));
      }

      /* Try call. */
      virtual void operator()(const TTryStmt *stmt) const override {
        assert(this);
        assert(stmt);
        TUuid pov_id = Translate(stmt->GetPovId());
        vector<string> fq_name;
        TranslatePathName(fq_name, stmt->GetPackage());
        TClosure closure(stmt->GetMethodName()->GetLexeme().GetText());
        auto list = dynamic_cast<const TObjMemberList *>(stmt->GetArgs()->GetOptObjMemberList());
        void *alloc = alloca(SabotStateSize);
        while (list) {
          auto member = list->GetObjMember();
          TWrapper state(NewStateSabot(member->GetExpr(), alloc));
          closure.AddArgBySabot(member->GetName()->GetLexeme().GetText(), state);
          auto tail = dynamic_cast<const TObjMemberListTail *>(list->GetOptObjMemberListTail());
          list = tail ? tail->GetObjMemberList() : nullptr;
        }
        TMethodResult result = GetSession()->Try(TMethodRequest(pov_id, fq_name, closure));
        void *state_alloc = alloca(Sabot::State::GetMaxStateSize());
        Result = AsStrFunc(
            &Var::Jsonify,
            Var::ToVar(*TWrapper(Indy::TKey(result.GetValue(), result.GetArena().get()).GetState(state_alloc))));
      }

      /* Pause or unpause a pov. */
      virtual void operator()(const TPovStatusStmt *stmt) const override {
        assert(this);
        assert(stmt);
        bool is_pause = dynamic_cast<const TPauseKind *>(stmt->GetStatusKind()) != nullptr;
        TUuid pov_id = Translate(stmt->GetIdExpr());
        if (is_pause) {
          GetSession()->PausePov(pov_id);
          Result = "paused";
        } else {
          GetSession()->UnpausePov(pov_id);
          Result = "unpaused";
        }
      }

      /* Tail the global pov. */
      virtual void operator()(const TTailStmt *) const override {
        assert(this);
        GetSession()->Tail();
      }

      /* Begin bulk import mode. */
      virtual void operator()(const TBeginImportStmt *) const override {
        assert(this);
        GetSession()->BeginImport();
      }

      /* End bulk import mode. */
      virtual void operator()(const TEndImportStmt *) const override {
        assert(this);
        GetSession()->EndImport();
      }

      /* Import bulk data. */
      virtual void operator()(const TImportStmt *stmt) const override {
        assert(this);
        assert(stmt);
        string path = Translate(stmt->GetFile());
        int64_t
            load_threads = Translate(stmt->GetLoadThreads()),
            merge_threads = Translate(stmt->GetMergeThreads()),
            merge_sim = Translate(stmt->GetMergeSim());
        GetSession()->Import(path, load_threads, merge_threads, merge_sim);
      }

      /* Compile Orlyscript into an installable package. */
      virtual void operator()(const TCompileStmt *stmt) const override {
        assert(this);
        assert(stmt);
        ostringstream out_strm;
        Result = TJson::Object;
        try {
          TTmpCopyToFile tmp_file(
              Conn->Ws->TmpDirMaker.GetPath(), Translate(stmt->GetStrExpr()),
              "tmp_pkg_", ".orly");
          /* Having just made the full path to the temp file and already knowing
             the dir in which to place the compiler outputs, we must now parse these
             apart again and reformat them to suit the JHM file name classes which
             the compiler uses.  This is ugly. */
          const auto &tmp_path = tmp_file.GetPath();
          auto last_pos = tmp_path.find_last_of('/');
          Jhm::TAbsPath abs_path_to_src(
              Jhm::TAbsBase(tmp_path.substr(0, last_pos)),
              Jhm::TRelPath(tmp_path.substr(last_pos + 1)));
          Jhm::TAbsBase abs_base_to_dir(
              Conn->Ws->SessionManager->GetPackageDir());
          auto pkg = Compiler::Compile(
              abs_path_to_src, abs_base_to_dir,
              true, true, false, out_strm);
          Result["status"] = "ok";
          Result["name"] = pkg.Name.ToRelPath().AsStr();
          Result["version"] = pkg.Version;
        } catch (const Compiler::TCompileFailure &ex) {
          Result["status"] = "error";
          Result["kind"] = "compiler";
          Result["diagnostics"] = out_strm.str();
        } catch (const TSourceError &src_error) {
          Result["status"] = "error";
          Result["kind"] = "compiler internal";
          Result["msg"] = src_error.what();
          Result["pos"] = src_error.GetPosRange().AsStr();
        } catch (const Base::TError &ex) {
          const auto &code_loc = ex.GetCodeLocation();
          Result["status"] = "error";
          Result["kind"] = "server system";
          Result["msg"] = ex.what();
          Result["file"] = code_loc.GetFile();
          Result["line"] = code_loc.GetLineNumber();
        } catch (const exception &ex) {
          Result["status"] = "error";
          Result["kind"] = "std exception";
          Result["msg"] = ex.what();
        }
      }

      virtual void operator()(const TListPackageStmt *) const override {
        assert(this);
        TJson::TArray packages;
        auto &package_manager = Conn->Ws->SessionManager->GetPackageManager();
        package_manager.YieldInstalled([&packages, &package_manager](const Package::TVersionedName &versioned_name) {
          TJson::TObject package_info;
          package_info["name"] = versioned_name.Name.AsStr();
          package_info["version"] = versioned_name.Version;

          /* get each function's info */ {
            TJson::TObject functions;
            package_manager.Get(versioned_name.Name)
                ->ForEachFunction([&functions](const string &name, auto func) {
              TJson::TObject func_info;
              /* params */ {
                TJson::TObject parameters;
                for (const auto &param: func->GetParameters()) {
                  // TODO: Change to jsonify
                  ostringstream oss;
                  Orly::Type::Orlyify(oss, param.second);
                  parameters[param.first] = oss.str();
                }

                func_info["parameters"] = TJson(move(parameters));
              }
              /* oss for return */ {
                ostringstream oss;
                Orly::Type::Orlyify(oss, func->GetReturnType());
                func_info["return"] = oss.str();
              }
              functions[name] = TJson(move(func_info));
              return true;
                  });

            package_info["functions"] = TJson(move(functions));
          }

          packages.emplace_back(move(package_info));
          return true;
        });
        Result = TJson::Object;
        Result["packages"] = TJson(move(packages));
      }

      virtual void operator()(const TGetSourceStmt *stmt) const override {
        assert(this);
        assert(stmt);

        vector<string> package_name;
        TranslatePathName(package_name, stmt->GetNameList());

        auto rel_path = Orly::Package::TName(package_name).ToRelPath({"orly"});
        string src_filename = AsStr(Jhm::TAbsPath(Conn->Ws->SessionManager->GetPackageDir(), rel_path));
        syslog(LOG_INFO, "filename = \"%s\"", src_filename.c_str());
        Result = TJson::Object;
        Result["code"] = ReadAll(TFd(open(src_filename.c_str(), O_RDONLY)));
        Result["filename"] = AsStr(rel_path);
      }

      private:

      /* The size used to alloca() space for a sabot of state. */
      static constexpr auto SabotStateSize = Orly::Sabot::State::GetMaxStateSize();

      /* Pull in some stuff from sabot. */
      using TStateDumper = Orly::Sabot::TStateDumper;
      using TWrapper = Orly::Sabot::State::TAny::TWrapper;

      /* The session we are using in this connection.  Never null.
         If no session has yet been established for this connection, throw. */
      TSessionPin *GetSession() const {
        assert(this);
        if (!Conn->Session) {
          throw invalid_argument("session not yet established");
        }
        return Conn->Session.get();
      }

      /* Translate an Orlyscript id into a TUuid. */
      static TUuid Translate(const TIdExpr *id_expr) {
        assert(id_expr);
        return TUuid(id_expr->GetLexeme().GetText().substr(1, id_expr->GetLexeme().GetText().size() - 2).c_str());
      }

      /* Translate an Orlyscript int into a 64-bit signed int. */
      static int64_t Translate(const TIntExpr *int_expr) {
        assert(int_expr);
        return int_expr->GetLexeme().AsInt();
      }

      /* Translate an Orlyscript string into a std string. */
      static string Translate(const TStrExpr *str_expr) {
        assert(str_expr);
        struct visitor_t final : public TStrExpr::TVisitor {
          string &Result;
          visitor_t(string &result) : Result(result) {}
          virtual void operator()(const TDoubleQuotedRawStrExpr *that) const override {
            Result = that->GetLexeme().AsDoubleQuotedRawString();
          }
          virtual void operator()(const TDoubleQuotedStrExpr *that) const override {
            Result = that->GetLexeme().AsDoubleQuotedString();
          }
          virtual void operator()(const TSingleQuotedRawStrExpr *that) const override {
            Result = that->GetLexeme().AsSingleQuotedRawString();
          }
          virtual void operator()(const TSingleQuotedStrExpr *that) const override {
            Result = that->GetLexeme().AsSingleQuotedString();
          }
        };
        string result;
        str_expr->Accept(visitor_t(result));
        return move(result);
      }

      /* The connection on whose behalf we're translating. */
      TConn *Conn;

      /* The JSON blob to which to write the result of our interpretation. */
      TJson &Result;

    };  // TWsImpl::TStmtVisitor

    /* The server of which this connection is a part. */
    TWsImpl *Ws;

    /* The handle of this connection, used to identify its TCP socket. */
    TConnHndl Hndl;

    /* See accessor. */
    bool Exiting;

    /* The session established for this connection, if any. */
    unique_ptr<TSessionPin> Session;

  };  // TWsImpl::TConn

  /* Called by the ws framework when it accepts a new TCP connection. */
  void OnOpen(TConnHndl conn_hndl) {
    assert(this);
    lock_guard<mutex> lock(Mutex);
    if (!Conns.insert(make_pair(conn_hndl, make_shared<TConn>(this, conn_hndl))).second) {
      throw invalid_argument("cannot reopen open connection");
    }
  }

  /* Called by the ws framework when it closes a TCP connection (or when the
     connection is closed by the client). */
  void OnClose(TConnHndl conn_hndl) {
    assert(this);
    lock_guard<mutex> lock(Mutex);
    auto iter = Conns.find(conn_hndl);
    if (iter == Conns.end()) {
      throw invalid_argument("cannot close unopen connection");
    }
    Conns.erase(iter);
  }

  /* Called by the ws framework each time a message arrives. */
  void OnMsg(TConnHndl conn_hndl, TMsgPtr msg) {
    assert(this);
    /* Find the connection object established by the OnOpen() call. */
    shared_ptr<TConn> conn;
    /* extra */ {
      lock_guard<mutex> lock(Mutex);
      auto iter = Conns.find(conn_hndl);
      if (iter == Conns.end()) {
        throw invalid_argument("cannot use unopen connection");
      }
      conn = iter->second;
    }
    /* Pass the message to the connection object for processing. */
    TJson reply = TJson::Object;
    try {
      reply["result"] = conn->OnMsg(msg);
      reply["status"] = "ok";
    } catch (const exception &ex) {
      reply["result"] = ex.what();
      reply["status"] = "exception";
    }
    /* Format the reply and send it back to the client. */
    WsServer.send(conn_hndl, AsStr(reply), websocketpp::frame::opcode::text);
    if (conn->IsExiting()) {
      WsServer.close(conn_hndl, websocketpp::close::status::normal, "");
    }
  }

  /* The session manager interface passed to us at construction time. */
  TSessionManager *SessionManager;

  /* Creates and destroys the tmp dir used by the compile stmt. */
  TTmpDirMaker TmpDirMaker;

  /* This object handles the mechanics of the websocket protocol. */
  TWsServer WsServer;

  /* The background thread launched by the ctor. */
  vector<thread> BgThreads;

  /* Convers Conns. */
  mutex Mutex;

  /* The sessions currently open. */
  map<TConnHndl, shared_ptr<TConn>, owner_less<TConnHndl>> Conns;

};  // TWsImpl

TWs *TWs::New(
    TSessionManager *session_mngr, size_t thread_count,
    in_port_t port_number) {
  return new TWsImpl(session_mngr, thread_count, port_number);
}
