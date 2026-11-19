#include "Endpoint.h"
#include "Heater.h"
#include "Max31855Thermocouple.h"
#include "uart/UartTransport.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include <cassert>
#include <limits>
#include <memory>
#include <vector>

uint32_t testMillis=1000;
bool heaterPinHigh=false;
std::function<void()> delayHook;
std::function<void()> pinHighHook;
float testRawTemperature=20;
uint8_t testTemperatureStatus=STATUS_OK;
float TemperatureSensor::read() { return 0; }
bool TemperatureSensor::isErrorState() { return false; }
void TemperatureSensor::setup() {}
struct Sensor : TemperatureSensor {
 float value=20; float raw=20; bool usable=true;
 float readForSafety() override { return raw; }
 float read() override { return value; }
 bool hasUsableReading() override { return usable; }
};

static void install(Heater &heater) {
 assert(heater.configure(30, 0.3f, 100, 0));
 heater.loop();
 assert(heater.hasConfiguration());
}
static void testHeater() {
 Sensor sensor;
 unsigned faults=0, results=0;
 Heater heater(&sensor,1,[&]{++faults;},[&](float,float,float,float){++results;});
 heater.setup();
 heater.setSetpoint(85);
 testMillis+=1000; heater.loop();
 assert(!heaterPinHigh && !heater.hasConfiguration());
 install(heater);
 assert(heater.getSetpoint()==0); // Pre-configuration targets cannot survive.
 heater.setSetpoint(85);
 testMillis+=1000; heater.loop();
 assert(heaterPinHigh);
 // Carry a saturated old integrator/output into a warm restart/target drop.
 heater.output=1000;
 heater.simplePid->feedback_integralState=42;
 sensor.value=110;
 heater.loop();
 assert(!heaterPinHigh && heater.output==0 && heater.getSetpoint()==85);
 assert(heater.simplePid->feedback_integralState==42); // Ordinary cutoff holds learned PID state.
 heater.setSetpoint(80);heater.loop();
 assert(heater.simplePid->feedback_integralState==0); // Changing the target discards the old demand.
 heater.setSetpoint(85);
 sensor.value=80; sensor.raw=85; heater.output=1000; heater.loop();
 assert(!heaterPinHigh && heater.output==0);
 sensor.value=20; sensor.raw=20; heater.setSetpoint(85); testMillis+=1000; heater.loop();
 assert(heaterPinHigh);
 heater.invalidateConfiguration();
 assert(!heaterPinHigh && !heater.hasConfiguration() && heater.getSetpoint()==0);
 heater.setSetpoint(145); heater.loop();
 assert(!heaterPinHigh);
 install(heater); heater.loop();
 assert(!heaterPinHigh && heater.getSetpoint()==0);
 heater.setSetpoint(85); testMillis+=1000; heater.loop();
 assert(heaterPinHigh);
 testMillis+=Heater::TARGET_LEASE_MS+1; heater.loop();
 assert(!heaterPinHigh && heater.output==0);
 heater.setSetpoint(85); sensor.usable=false; heater.loop();
 assert(!heaterPinHigh && heater.output==0);
 sensor.usable=true;
 assert(!heater.configure(std::numeric_limits<float>::quiet_NaN(),1,1,0));
 assert(!heater.hasConfiguration() && !heaterPinHigh);
 assert(!heater.configure(-1,1,1,0));
 assert(!heater.configure(0,0,0,0));
 install(heater);
 heater.setSetpoint(std::numeric_limits<float>::quiet_NaN()); heater.loop();
 assert(!heaterPinHigh);
 heater.setSetpoint(85); heater.output=std::numeric_limits<float>::quiet_NaN();
 heater.softPwm(1000); assert(!heaterPinHigh);
 // Pin shutoff must be immediate at zero duty, including millisecond rollover.
 testMillis=0xfffffff0u; heater.setSetpoint(85); heater.output=1000; heater.softPwm(1000);
 assert(heaterPinHigh);
 testMillis=10; heater.output=0; heater.softPwm(1000); assert(!heaterPinHigh);
 heater.output=1000;
 pinHighHook=[&]{ heater.setSetpoint(0); };
 heater.softPwm(1000); assert(!heaterPinHigh); // A concurrent stop cannot be overwritten by PWM.
 heater.setSetpoint(85); sensor.raw=90; heater.output=1000;
 heater.softPwm(1000); assert(!heaterPinHigh); // Raw temperature is checked even between PID updates.
 sensor.raw=20;
 heater.autotune(120,6,1200);
 delayHook=[&]{ delayHook=nullptr; heater.invalidateConfiguration(); };
 heater.loop();
 assert(!heaterPinHigh && !heater.hasConfiguration() && results==0);
 assert(faults==0);
 puts("PASS heater: configuration/fresh-target gating, stale lease, 85 C cutoff, warm restart, invalid inputs, PWM rollover and autotune cancellation");
}
static void testSensor() {
 unsigned faults=0;
 Max31855Thermocouple sensor(1,2,3,[](float){},[&]{++faults;});
 sensor.setup();
 assert(!sensor.hasUsableReading());
 testRawTemperature=120; sensor.loop();
 assert(sensor.hasUsableReading() && sensor.read()==120); // Never initialize a hot boiler as 24 C.
 testTemperatureStatus=STATUS_NOREAD; sensor.loop();
 assert(!sensor.hasUsableReading());
 testTemperatureStatus=STATUS_OK; sensor.loop();
 assert(sensor.hasUsableReading());
 testMillis+=1001; assert(!sensor.hasUsableReading());
 testRawTemperature=std::numeric_limits<float>::quiet_NaN(); sensor.loop(); assert(!sensor.hasUsableReading());
 testRawTemperature=175; sensor.loop(); assert(faults>0);
 puts("PASS sensor: first hot reading, failed read, stale read and raw overtemperature");
}

struct Link : Transport {
 bool up=true;
 std::deque<std::vector<uint8_t>> outgoing;
 bool send(uint8_t const *data,size_t length) override {
   assert(length<=256); outgoing.emplace_back(data,data+length); return true;
 }
 bool isConnected() const override { return up; }
 size_t maxDatagram() const override { return 256; }
 void connect() { emitConnection(true); }
 void inject(std::vector<uint8_t> const &bytes) { emitData(bytes.data(),bytes.size()); }
};
static gm::Frame decode(std::vector<uint8_t> const &bytes) {
 gm::Frame frame=gaggimate_Frame_init_zero;
 auto input=pb_istream_from_buffer(bytes.data(),bytes.size());
 assert(pb_decode(&input,&gaggimate_Frame_msg,&frame)); return frame;
}
static std::vector<uint8_t> encode(gm::Frame const &frame) {
 std::vector<uint8_t> bytes(256);
 auto output=pb_ostream_from_buffer(bytes.data(),bytes.size());
 assert(pb_encode(&output,&gaggimate_Frame_msg,&frame)); bytes.resize(output.bytes_written); return bytes;
}
static void dispatchAll(Endpoint &endpoint) {
 Endpoint::DispatchEvent event;
 while(xQueueReceive(endpoint._rxQueue,&event,0)==pdTRUE) endpoint.processDispatchEvent(event);
}
static void settle(Endpoint &a, Link &la, Endpoint &b, Link &lb, unsigned loss=0) {
 for(unsigned i=0;i<100;++i) {
   while(!la.outgoing.empty()) { auto bytes=la.outgoing.front();la.outgoing.pop_front(); if(loss) --loss; else lb.inject(bytes); }
   while(!lb.outgoing.empty()) { auto bytes=lb.outgoing.front();lb.outgoing.pop_front(); if(loss) --loss; else la.inject(bytes); }
   dispatchAll(a);dispatchAll(b);testMillis+=20;a.loop();b.loop();
   if(a.isConnected()&&b.isConnected()&&la.outgoing.empty()&&lb.outgoing.empty()&&!a._inFlight&&!b._inFlight) return;
 }
 assert(a.isConnected()&&b.isConnected());
}
static gm::Payload boiler(float target) {
 gm::Payload p=gaggimate_Payload_init_zero; p.which_content=gaggimate_Payload_boiler_tag;
 p.content.boiler.setpoint=target; return p;
}
static void testSessions() {
 Link la,lb;
 auto a=std::make_unique<Endpoint>(la), b=std::make_unique<Endpoint>(lb);
 unsigned received=0, boundaries=0; float lastTarget=0;
 auto attach=[&]{
   b->onConnection([&](bool){++boundaries;});
   b->on(gaggimate_Payload_boiler_tag,[&](gm::Payload const &p){++received;lastTarget=p.content.boiler.setpoint;});
 };
 a->begin();b->begin();attach();la.connect();lb.connect();
 settle(*a,la,*b,lb,5);
 assert(a->isConnected()&&b->isConnected());
 a->send(boiler(145));a->loop();
 assert(!la.outgoing.empty());auto oldCommand=la.outgoing.front();
 lb.inject(oldCommand);la.outgoing.clear();dispatchAll(*b);
 assert(received==1 && lastTarget==145);
 auto oldAck=lb.outgoing.front();settle(*a,la,*b,lb);
 // Duplicate retransmissions remain idempotent inside the same session.
 lb.inject(oldCommand);dispatchAll(*b);assert(received==1);settle(*a,la,*b,lb);
 const auto oldPeer=b->_localSession;
 a.reset(); a=std::make_unique<Endpoint>(la); a->begin();la.connect();
 settle(*a,la,*b,lb);
 assert(b->_localSession!=oldPeer && boundaries>=4);
 a->send(boiler(85));a->loop();assert(a->_inFlight);
 la.inject(oldAck);assert(a->_inFlight); // An ACK from the prior boot must not consume frame 1.
 lb.inject(oldCommand);dispatchAll(*b);assert(received==1);
 settle(*a,la,*b,lb);assert(received==2 && lastTarget==85);
 // The reverse partial restart also resynchronizes without a transport outage.
 const auto before=b->_localSession;
 b.reset();b=std::make_unique<Endpoint>(lb);b->begin();attach();lb.connect();
 settle(*a,la,*b,lb);
 a->send(boiler(85));settle(*a,la,*b,lb);
 assert(received==3 && lastTarget==85 && b->_localSession!=before);
 // Neither an unaddressed payload nor a legacy frame may actuate outputs.
 gm::Frame invalid=gaggimate_Frame_init_zero;
 invalid.id=100;invalid.payloads_count=1;invalid.payloads[0]=boiler(145);
 lb.inject(encode(invalid));dispatchAll(*b);assert(received==3);
 invalid.session_id=a->_localSession;invalid.peer_session_id=0;
 lb.inject(encode(invalid));dispatchAll(*b);assert(received==3);
 settle(*a,la,*b,lb);
 // Fill the queue with six distinct, maximum-length payload families. The
 // encoder must split oversized batches while keeping every datagram <=256.
 for(int kind=0;kind<6;++kind) {
   gm::Payload p=gaggimate_Payload_init_zero;
   p.which_content=static_cast<pb_size_t>(gaggimate_Payload_system_info_tag+kind);
   if(kind==0) {
     auto &info=p.content.system_info;
     std::memset(info.hardware,'H',sizeof(info.hardware)-1);
     std::memset(info.version,'V',sizeof(info.version)-1);
     info.has_capabilities=true;info.capabilities.addons_count=4;
     for(auto &addon:info.capabilities.addons) addon.type=12345678;
   } else if(kind==1) {
     p.which_content=gaggimate_Payload_led_tag;p.content.led.channels_count=8;
     for(int j=0;j<8;++j) {p.content.led.channels[j].channel=j;p.content.led.channels[j].brightness=255;}
   } else if(kind==2) {
     p.which_content=gaggimate_Payload_sensor_tag;p.content.sensor.boilers_count=4;
     for(auto &reading:p.content.sensor.boilers) {
       reading.temperature=85;reading.pressure=9;reading.power=100;reading.target_temperature=85;reading.heating_configured=true;
     }
   } else if(kind==3) { p.which_content=gaggimate_Payload_pump_model_tag;p.content.pump_model.a=10;p.content.pump_model.b=11; }
   else if(kind==4) {p.which_content=gaggimate_Payload_scale_sample_tag;p.content.scale_sample.weight_g=10;}
   else { p=boiler(85); }
   a->send(p);
 }
 settle(*a,la,*b,lb);
 assert(a->_queue.empty()&&!a->_inFlight);
 std::array<gm::Payload, 7> stopBatch{};
 std::array<bool, 7> receivedStop{};
 b->on(gaggimate_Payload_relay_tag, [&](gm::Payload const &p) {
    const auto &relay = p.content.relay;
    assert(relay.index < receivedStop.size() && !relay.open);
    receivedStop[relay.index] = true;
 });
 for (size_t i = 0; i < stopBatch.size(); ++i) {
    stopBatch[i].which_content = gaggimate_Payload_relay_tag;
    stopBatch[i].content.relay.index = i;
 }
 gm::Payload staleRelay = stopBatch.back();
 staleRelay.content.relay.open = true;
 a->send(staleRelay);
 a->loop();
 la.outgoing.clear(); // Stop supersedes an unacknowledged command, including one queued in a second frame.
 a->sendUrgentBatch(stopBatch.data(), stopBatch.size());
 settle(*a, la, *b, lb);
 assert(std::all_of(receivedStop.begin(), receivedStop.end(), [](bool received) { return received; }));
 puts("PASS sessions: loss/retry, independent ESP/STM restarts, duplicate commands, stale ACKs/targets, missing session IDs and bounded frames");
}
static void testHeatingAcrossSessions() {
 Sensor sensor;
 Heater heater(&sensor,1,[]{ assert(false); },[](float,float,float,float){});
 heater.setup();
 Link displayLink, controllerLink;
 auto display=std::make_unique<Endpoint>(displayLink);
 auto controller=std::make_unique<Endpoint>(controllerLink);
 auto attach=[&]{
   controller->onConnection([&](bool){ heater.invalidateConfiguration(); });
   controller->on(gaggimate_Payload_heater_tag,[&](gm::Payload const &p){
     auto const &pid=p.content.heater;
     heater.configure(pid.kp,pid.ki,pid.kd,pid.kf);
   });
   controller->on(gaggimate_Payload_boiler_tag,[&](gm::Payload const &p){ heater.setSetpoint(p.content.boiler.setpoint); });
 };
 display->begin();controller->begin();attach();displayLink.connect();controllerLink.connect();
 settle(*display,displayLink,*controller,controllerLink);
 gm::Payload pid=gaggimate_Payload_init_zero;
 pid.which_content=gaggimate_Payload_heater_tag;pid.content.heater.kp=30;pid.content.heater.ki=0.3f;pid.content.heater.kd=100;
 gm::Payload initial[]={pid,boiler(85)};
 display->sendBatch(initial,2);settle(*display,displayLink,*controller,controllerLink);heater.loop();
 assert(heater.hasConfiguration() && heater.getSetpoint()==0 && !heaterPinHigh);
 display->send(boiler(85));settle(*display,displayLink,*controller,controllerLink);
 testMillis+=1000;heater.loop();assert(heaterPinHigh);
 display->send(boiler(145));auto staleSteam=displayLink.outgoing.front();displayLink.outgoing.clear();
 // Reboot the display without dropping the UART wire or restarting the heater.
 display.reset();display=std::make_unique<Endpoint>(displayLink);display->begin();displayLink.connect();
 settle(*display,displayLink,*controller,controllerLink);
 assert(!heaterPinHigh && !heater.hasConfiguration());
 controllerLink.inject(staleSteam);dispatchAll(*controller);
 assert(!heaterPinHigh && heater.getSetpoint()==0);
 display->send(boiler(85));settle(*display,displayLink,*controller,controllerLink);heater.loop();
 assert(!heaterPinHigh && !heater.hasConfiguration());
 display->send(pid);settle(*display,displayLink,*controller,controllerLink);heater.loop();
 assert(heater.hasConfiguration() && heater.getSetpoint()==0);
 display->send(boiler(85));settle(*display,displayLink,*controller,controllerLink);
 testMillis+=1000;heater.loop();assert(heaterPinHigh);
 // A warm controller restart also requires configuration and a fresh target.
 sensor.value=110;sensor.raw=110;
 controller.reset();controller=std::make_unique<Endpoint>(controllerLink);controller->begin();attach();controllerLink.connect();
 settle(*display,displayLink,*controller,controllerLink);assert(!heaterPinHigh && !heater.hasConfiguration());
 display->send(pid);settle(*display,displayLink,*controller,controllerLink);heater.loop();
 display->send(boiler(85));settle(*display,displayLink,*controller,controllerLink);heater.loop();
 assert(heater.hasConfiguration() && heater.getSetpoint()==85 && !heaterPinHigh);
 puts("PASS integration: partial board restarts inhibit heat; stale steam and pre-configuration targets cannot energize a warm boiler");
}
class MemorySerial : public Stream {
  public:
    MemorySerial *peer = nullptr;
    std::deque<uint8_t> input;

    int available() override { return input.size(); }
    int read() override {
        if (input.empty())
            return -1;
        const uint8_t next = input.front();
        input.pop_front();
        return next;
    }
    size_t write(const uint8_t *data, size_t length) override {
        if (peer)
            peer->input.insert(peer->input.end(), data, data + length);
        return length;
    }
};

static void testUart() {
    MemorySerial display, controller;
    UartTransport a(display), b(controller);
    display.peer = &controller;
    a.begin();
    b.begin();
    const auto advance = [&] {
        for (int i = 0; i < 30; ++i) {
            testMillis += 50;
            a.loop();
            b.loop();
        }
    };
    advance();
    assert(!a.isConnected() && !b.isConnected());
    controller.peer = &display;
    advance();
    assert(a.isConnected() && b.isConnected());
    std::vector<uint8_t> received;
    b.onData([&](const uint8_t *data, size_t length) { received.assign(data, data + length); });
    const std::array<uint8_t, 7> payload{0, 255, 1, 0, 0, 4, 0};
    assert(a.send(payload.data(), payload.size()));
    controller.input[2] ^= 0x80;
    b.loop();
    assert(received.empty());
    assert(a.send(payload.data(), payload.size()));
    b.loop();
    assert(std::equal(received.begin(), received.end(), payload.begin(), payload.end()));
    b.begin(); // Reset one transport while the display remains powered.
    advance();
    assert(a.isConnected() && b.isConnected());
    puts("PASS UART: bidirectional handshake, CRC rejection, embedded zeros, independent restart");
}

int main() {
    testHeater();
    testSensor();
    testSessions();
    testHeatingAcrossSessions();
    testUart();
}
