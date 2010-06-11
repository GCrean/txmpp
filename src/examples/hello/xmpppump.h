#ifndef _HELLO_XMPPPUMP_H_
#define _HELLO_XMPPPUMP_H_

#include "../../messagequeue.h"
#include "../../taskrunner.h"
#include "../../thread.h"
#include "../../time.h"
#include "../../xmppclient.h"
#include "../../xmppengine.h"
#include "../../xmpptask.h"

namespace hello {

class XmppPumpNotify {
  public:
    virtual ~XmppPumpNotify() {}
    virtual void OnStateChange(txmpp::XmppEngine::State state) = 0;
};

class XmppPump : public txmpp::MessageHandler, public txmpp::TaskRunner {
  public:
    XmppPump(XmppPumpNotify * notify = NULL);

    txmpp::XmppClient *client() { return client_; }
    txmpp::XmppReturnStatus SendStanza(const txmpp::XmlElement *stanza);
    int64 CurrentTime();

    void DoLogin(const txmpp::XmppClientSettings & xcs,
                 txmpp::XmppAsyncSocket* socket,
                 txmpp::PreXmppAuth* auth);
    void DoDisconnect();
    void WakeTasks();

    void OnStateChange(txmpp::XmppEngine::State state);
    void OnMessage(txmpp::Message *pmsg);

  private:
    txmpp::XmppClient *client_;
    txmpp::XmppEngine::State state_;
    XmppPumpNotify *notify_;
};

}  // namespace hello

#endif  // _HELLO_XMPPPUMP_H_
