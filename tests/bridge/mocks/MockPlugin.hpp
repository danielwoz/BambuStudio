// Bambu Bridge — MockPlugin (harness).
//
// Subclass of BambuNetworkingPluginHandle that answers the cloud-side
// MQTT and REST surface the way a real A1 / H2S / H2D would, but
// in-process. Each registered device drives its own PrinterFsm; the
// MockPlugin parses inbound publish_to_device commands, transitions
// the FSM, and emits push_status replies via the base class's
// deliver_message_for_test() seam.
//
// Concurrency model: the FSM and the response queue live behind one
// mutex per device. Tests can drive the plugin synchronously (preferred)
// or attach an internal worker thread that fires periodic push_status
// updates while the FSM is in the Running state (opt-in via
// start_ticker()). v1 keeps the ticker OFF — every push_status is the
// product of an explicit caller action.

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_MOCK_PLUGIN_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_MOCK_PLUGIN_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "BambuNetworkingPluginHandle.hpp"
#include "PrinterFsm.hpp"
#include "PrinterModel.hpp"

namespace Slic3r {
namespace bridge {
namespace mocks {

// Polymorphic shim around the per-model A1/H2S/H2D classes. Each
// concrete printer exposes `build_push_status()` returning a JSON
// payload tied to its current FSM state, plus a reference to that FSM.
class IPrinterBehaviour {
public:
    virtual ~IPrinterBehaviour() = default;
    virtual const std::string&  dev_id()              const = 0;
    virtual const PrinterModel& model()               const = 0;
    virtual PrinterFsm&         fsm()                       = 0;
    virtual std::string         build_push_status_str()    = 0;
};

// Concrete adapter: any class with `dev_id()/model()/fsm()/
// build_push_status()` matching the signatures in A1Printer/H2SPrinter/
// H2DPrinter works. The adapter takes ownership of the underlying
// printer struct.
template <class P>
class PrinterAdapter : public IPrinterBehaviour {
public:
    explicit PrinterAdapter(std::shared_ptr<P> printer)
        : m_p(std::move(printer)) {}

    const std::string&  dev_id() const override { return m_p->dev_id(); }
    const PrinterModel& model()  const override { return m_p->model(); }
    PrinterFsm&         fsm()          override { return m_p->fsm(); }
    std::string build_push_status_str() override {
        return m_p->build_push_status().dump();
    }

private:
    std::shared_ptr<P> m_p;
};

class MockPlugin : public BambuNetworkingPluginHandle {
public:
    MockPlugin();
    ~MockPlugin() override = default;

    // Wire up a printer behaviour ahead of the test. The dev_id used to
    // address the printer comes from `behaviour->dev_id()`; same dev_id
    // is what the bridge subscribes/publishes against.
    void add_printer(std::shared_ptr<IPrinterBehaviour> behaviour);

    // ---- BambuNetworkingPluginHandle overrides ---------------------------
    bool init()                  override { return true; }
    bool agent_ready()     const override { return true;  }
    bool is_user_login()   const override { return true;  }
    bool is_server_connected() const override { return m_server_up.load(); }

    bool get_user_print_info(unsigned int* http_code,
                             std::string*  http_body) const override;

    int subscribe_device  (const std::string& dev_id) override;
    int unsubscribe_device(const std::string& dev_id) override;

    int publish_to_device (const std::string& dev_id,
                           const std::string& json_payload,
                           int                qos) override;

    int get_camera_url(const std::string& dev_id,
                       std::string*       url_out,
                       int                timeout_ms = 10000) override;

    // ---- Test introspection ---------------------------------------------
    //
    // Tests assert against these to confirm the bridge crossed the seam
    // the expected number of times.
    struct PublishRecord {
        std::string dev_id;
        std::string json;
        int         qos;
    };
    struct Subscription {
        std::string dev_id;
        bool        active;
    };

    std::vector<PublishRecord> publishes_for(const std::string& dev_id) const;
    std::vector<Subscription>  subscriptions() const;
    int  publish_count() const;

    // Toggle the simulated cloud-broker connectivity (drives
    // is_server_connected()). Default = true.
    void set_server_up(bool up) { m_server_up.store(up); }

private:
    IPrinterBehaviour* find_printer_locked(const std::string& dev_id) const;
    void               emit_push_status(IPrinterBehaviour* p);

    mutable std::mutex                                m_mu;
    std::vector<std::shared_ptr<IPrinterBehaviour>>  m_printers;
    std::unordered_map<std::string, bool>            m_subscribed;
    std::vector<PublishRecord>                       m_publishes;
    std::atomic<bool>                                m_server_up{true};
};

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_MOCK_PLUGIN_HPP
