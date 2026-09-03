#ifdef _WIN32
#include <Winsock2.h>
#include <Ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include <LocalHost.hpp>
#include <AddressConversion.hpp>
#include <Logger.hpp>
#include <SpeedwireSocketFactory.hpp>
#include <SpeedwireHeader.hpp>
#include <SpeedwireTagHeader.hpp>
#include <SpeedwireEmeterProtocol.hpp>
#include <ObisData.hpp>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <cstring>
using namespace libspeedwire;

// susyids for different emeter device types
#define SUSYID_EMETER_10        (270)
#define SUSYID_EMETER_20        (349)
#define SUSYID_HOMEMANAGER_20   (372)

#define SUSYID  SUSYID_EMETER_20

// serial number of the device - choose an arbitrary number here, the combination of susyid and serialnumber must be unique in a given speedwire network
#define SERIAL_NUMBER (1901567274)

// sunny home manager in version 2.07.x.y used a different speedwire header (when unicast transmission was introduced)
#define USE_EXTENDED_EMETER_PROTOCOL (0)

// since firmware version 2.03.4.R a frequency measurement has been added to emeter packets
#define INCLUDE_FREQUENCY_MEASUREMENT (1)

#if INCLUDE_FREQUENCY_MEASUREMENT && USE_EXTENDED_EMETER_PROTOCOL
  #define UDP_PACKET_SIZE 610
  #define PROTOCOL_ID (SpeedwireData2Packet::sma_extended_emeter_protocol_id)
#elif INCLUDE_FREQUENCY_MEASUREMENT
  #define UDP_PACKET_SIZE 608
  #define PROTOCOL_ID (SpeedwireData2Packet::sma_emeter_protocol_id)
#else
  #define UDP_PACKET_SIZE 600
  #define PROTOCOL_ID (SpeedwireData2Packet::sma_emeter_protocol_id)
#endif

#if INCLUDE_FREQUENCY_MEASUREMENT
  #define FIRMWARE_VERSION    ("2.03.4.R")
#elif USE_EXTENDED_EMETER_PROTOCOL
  #define FIRMWARE_VERSION    ("2.07.4.R")
#else
  #define FIRMWARE_VERSION    ("2.0.18.R")
#endif

#define USE_MULTICAST_SCOCKET (1)

// udp port on which textual power values (Watt, signed; positive = consumption, negative = feed-in) are received
#define POWER_INPUT_UDP_PORT (8888)


static void* insert(SpeedwireEmeterProtocol& emeter_packet, void* const obis, const ObisData& obis_data, const double value);
static void* insert(SpeedwireEmeterProtocol& emeter_packet, void* const obis, const ObisData& obis_data, const std::string& value);

class LogListener : public ILogListener {
public:
    virtual ~LogListener() {}

    virtual void log_msg(const std::string& msg, const LogLevel& level) {
        fprintf(stdout, "%s", msg.c_str());
    }

    virtual void log_msg_w(const std::wstring& msg, const LogLevel& level) {
        fprintf(stdout, "%ls", msg.c_str());
    }
};

static Logger logger("main");

// latest total active power value (Watt) received via udp; positive = consumption, negative = feed-in
static std::atomic<double> received_power_watts(0.0);

// listens on POWER_INPUT_UDP_PORT for plain text power values and stores the latest one in received_power_watts
static void receivePowerValues(void) {
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
#else
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
#endif
        logger.print(LogLevel::LOG_ERROR, "cannot create udp receive socket\n");
        return;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(POWER_INPUT_UDP_PORT);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        logger.print(LogLevel::LOG_ERROR, "cannot bind udp receive socket to port %d\n", POWER_INPUT_UDP_PORT);
        return;
    }
    logger.print(LogLevel::LOG_INFO_0, "listening for power values on udp port %d\n", POWER_INPUT_UDP_PORT);

    char buffer[64];
    while (true) {
        sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);
        int nbytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&sender, &sender_len);
        if (nbytes > 0) {
            buffer[nbytes] = '\0';
            char* end = nullptr;
            double value = std::strtod(buffer, &end);
            if (end != buffer) {
                received_power_watts.store(value);
                logger.print(LogLevel::LOG_INFO_1, "received power value %.2f W\n", value);
            }
        }
    }
}


int main(int argc, char** argv) {

    // configure logger and logging levels
    ILogListener* log_listener = new LogListener();
    LogLevel log_level = LogLevel::LOG_ERROR | LogLevel::LOG_WARNING;
    log_level = log_level | LogLevel::LOG_INFO_0;
    log_level = log_level | LogLevel::LOG_INFO_1;
    log_level = log_level | LogLevel::LOG_INFO_2;
    log_level = log_level | LogLevel::LOG_INFO_3;
    Logger::setLogListener(log_listener, log_level);

    // configure sockets; use unicast socket to avoid messing around with igmp issues
    LocalHost &localhost = LocalHost::getInstance();
#if USE_MULTICAST_SCOCKET
    SpeedwireSocketFactory *socket_factory = SpeedwireSocketFactory::getInstance(localhost, SpeedwireSocketFactory::SocketStrategy::ONE_SINGLE_SOCKET);
#else
    SpeedwireSocketFactory* socket_factory = SpeedwireSocketFactory::getInstance(localhost, SpeedwireSocketFactory::SocketStrategy::ONE_UNICAST_SOCKET_FOR_EACH_INTERFACE);
#endif

    // start background thread receiving power values via udp
    std::thread power_receiver_thread(receivePowerValues);
    power_receiver_thread.detach();

    // define speedwire packet
    uint8_t udp_packet[UDP_PACKET_SIZE];
    SpeedwireHeader speedwire_packet(udp_packet, sizeof(udp_packet));

    // determine the emeter payload length by subtracting the tag header overhead of the default tag header structure
    unsigned long udp_header_length = speedwire_packet.getDefaultHeaderTotalLength(1, 0, 0);
    uint16_t udp_payload_length = (uint16_t)(UDP_PACKET_SIZE - udp_header_length);

    // create a tag header structure using the correct emeter payload length
    speedwire_packet.setDefaultHeader(1, udp_payload_length, PROTOCOL_ID);
    uint8_t *end_of_emeter_payload = (uint8_t*)speedwire_packet.findTagPacket(SpeedwireTagHeader::sma_tag_endofdata);

    SpeedwireData2Packet data2_packet(speedwire_packet);
    SpeedwireEmeterProtocol emeter_packet(data2_packet);
    emeter_packet.setSusyID(SUSYID);
    emeter_packet.setSerialNumber(SERIAL_NUMBER);
    emeter_packet.setTime((uint32_t)localhost.getUnixEpochTimeInMs());

    // energy accumulators (kWh) for the active power obis elements that are driven by the received udp power value;
    // they are integrated over time every loop iteration, starting from the same demo values as before
    double positive_active_energy_total = 1320.34, negative_active_energy_total = 305.03;
    double positive_active_energy_l1 = 337.53, negative_active_energy_l1 = 141.54;
    double positive_active_energy_l2 = 775.23, negative_active_energy_l2 = 77.80;
    double positive_active_energy_l3 = 271.21, negative_active_energy_l3 = 149.31;

    //
    // main loop
    //
    while (true) {

        // update timer
        uint32_t current_time = (uint32_t)localhost.getUnixEpochTimeInMs();
        emeter_packet.setTime(current_time);

        // determine current total active power (Watt) from the latest udp value; split evenly across all 3 phases;
        // sign determines consumption (positive) vs. feed-in (negative)
        double total_power = received_power_watts.load();
        double phase_power = total_power / 3.0;
        double pos_power_total = (total_power > 0.0) ? total_power : 0.0;
        double neg_power_total = (total_power < 0.0) ? -total_power : 0.0;
        double pos_power_phase = (phase_power > 0.0) ? phase_power : 0.0;
        double neg_power_phase = (phase_power < 0.0) ? -phase_power : 0.0;

        // integrate energy (kWh), assuming a 1000ms interval between loop iterations
        const double dt_hours = 1000.0 / 3600000.0;
        positive_active_energy_total += (pos_power_total / 1000.0) * dt_hours;
        negative_active_energy_total += (neg_power_total / 1000.0) * dt_hours;
        positive_active_energy_l1    += (pos_power_phase / 1000.0) * dt_hours;
        negative_active_energy_l1    += (neg_power_phase / 1000.0) * dt_hours;
        positive_active_energy_l2    += (pos_power_phase / 1000.0) * dt_hours;
        negative_active_energy_l2    += (neg_power_phase / 1000.0) * dt_hours;
        positive_active_energy_l3    += (pos_power_phase / 1000.0) * dt_hours;
        negative_active_energy_l3    += (neg_power_phase / 1000.0) * dt_hours;

        // insert all measurements available in an sma emeter packet into udp packet payload;
        // they are inserted in the same order as they are generated by an sme emeter device;
        // the order is important, as most open source projects do not parse obis elements 
        // but rather assume information at a given byte offset inside the udp packet.
        void* obis = (void*)emeter_packet.getFirstObisElement();

        // totals
        obis = insert(emeter_packet, obis, ObisData::PositiveActivePowerTotal,     pos_power_total);
        obis = insert(emeter_packet, obis, ObisData::PositiveActiveEnergyTotal,    positive_active_energy_total);
        obis = insert(emeter_packet, obis, ObisData::NegativeActivePowerTotal,     neg_power_total);
        obis = insert(emeter_packet, obis, ObisData::NegativeActiveEnergyTotal,    negative_active_energy_total);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactivePowerTotal,     0.00);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactiveEnergyTotal,    5.90);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactivePowerTotal,   188.90);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactiveEnergyTotal,  949.68);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentPowerTotal,   224.60);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentEnergyTotal, 1757.41);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentPowerTotal,     0.00);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentEnergyTotal,  327.62);
        obis = insert(emeter_packet, obis, ObisData::PowerFactorTotal,               0.54);
#if INCLUDE_FREQUENCY_MEASUREMENT
        obis = insert(emeter_packet, obis, ObisData::Frequency,                     50.16);
#endif

        // line 1
        obis = insert(emeter_packet, obis, ObisData::PositiveActivePowerL1,      pos_power_phase);
        obis = insert(emeter_packet, obis, ObisData::PositiveActiveEnergyL1,     positive_active_energy_l1);
        obis = insert(emeter_packet, obis, ObisData::NegativeActivePowerL1,      neg_power_phase);
        obis = insert(emeter_packet, obis, ObisData::NegativeActiveEnergyL1,     negative_active_energy_l1);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactivePowerL1,        0.00);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactiveEnergyL1,       2.48);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactivePowerL1,       22.30);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactiveEnergyL1,     176.48);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentPowerL1,        0.00);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentEnergyL1,     473.68);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentPowerL1,       31.10);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentEnergyL1,     144.26);
        obis = insert(emeter_packet, obis, ObisData::CurrentL1,                      0.18);
        obis = insert(emeter_packet, obis, ObisData::VoltageL1,                    231.97);
        obis = insert(emeter_packet, obis, ObisData::PowerFactorL1,                  0.70);

        // line 2
        obis = insert(emeter_packet, obis, ObisData::PositiveActivePowerL2,      pos_power_phase);
        obis = insert(emeter_packet, obis, ObisData::PositiveActiveEnergyL2,     positive_active_energy_l2);
        obis = insert(emeter_packet, obis, ObisData::NegativeActivePowerL2,      neg_power_phase);
        obis = insert(emeter_packet, obis, ObisData::NegativeActiveEnergyL2,     negative_active_energy_l2);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactivePowerL2,        0.00);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactiveEnergyL2,       7.38);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactivePowerL2,      126.00);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactiveEnergyL2,     535.19);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentPowerL2,      204.30);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentEnergyL2,     974.19);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentPowerL2,        0.00);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentEnergyL2,      89.10);
        obis = insert(emeter_packet, obis, ObisData::CurrentL2,                      1.12);
        obis = insert(emeter_packet, obis, ObisData::VoltageL2,                    230.66);
        obis = insert(emeter_packet, obis, ObisData::PowerFactorL2,                  0.79);

        // line 3
        obis = insert(emeter_packet, obis, ObisData::PositiveActivePowerL3,      pos_power_phase);
        obis = insert(emeter_packet, obis, ObisData::PositiveActiveEnergyL3,     positive_active_energy_l3);
        obis = insert(emeter_packet, obis, ObisData::NegativeActivePowerL3,      neg_power_phase);
        obis = insert(emeter_packet, obis, ObisData::NegativeActiveEnergyL3,     negative_active_energy_l3);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactivePowerL3,        0.00);
        obis = insert(emeter_packet, obis, ObisData::PositiveReactiveEnergyL3,       1.70);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactivePowerL3,       40.66);
        obis = insert(emeter_packet, obis, ObisData::NegativeReactiveEnergyL3,     243.67);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentPowerL3,        0.00);
        obis = insert(emeter_packet, obis, ObisData::PositiveApparentEnergyL3,     434.62);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentPowerL3,       44.30);
        obis = insert(emeter_packet, obis, ObisData::NegativeApparentEnergyL3,     156.83);
        obis = insert(emeter_packet, obis, ObisData::CurrentL3,                      0.23);
        obis = insert(emeter_packet, obis, ObisData::VoltageL3,                    230.09);
        obis = insert(emeter_packet, obis, ObisData::PowerFactorL3,                  0.40);

        // software version and end of data
        obis = insert(emeter_packet, obis, ObisData::SoftwareVersion, FIRMWARE_VERSION);

        // check if the packet is fully assembled
        if (obis != end_of_emeter_payload) {
            logger.print(LogLevel::LOG_ERROR, "invalid udp packet size %lu\n", (unsigned long)((uint8_t*)obis - udp_packet));
        }

        // send speedwire emeter packet to all local interfaces
        const std::vector<std::string>& localIPs = localhost.getLocalIPv4Addresses();
        for (auto& local_ip_addr : localIPs) {
#if USE_MULTICAST_SCOCKET
            SpeedwireSocket socket = SpeedwireSocketFactory::getInstance(localhost)->getSendSocket(SpeedwireSocketFactory::SocketType::MULTICAST, local_ip_addr);
            logger.print(LogLevel::LOG_INFO_0, "multicast sma emeter packet to %s (via interface %s)\n", AddressConversion::toString(socket.getSpeedwireMulticastIn4Address()).c_str(), local_ip_addr.c_str());
            int nbytes = socket.sendto(udp_packet, sizeof(udp_packet), socket.getSpeedwireMulticastIn4Address(), AddressConversion::toInAddress(local_ip_addr));
#else
            SpeedwireSocket& socket = socket_factory->getSendSocket(SpeedwireSocketFactory::SocketType::UNICAST, local_ip_addr);
            logger.print(LogLevel::LOG_INFO_0, "multicast sma emeter packet to %s (via interface %s)\n", AddressConversion::toString(socket.getSpeedwireMulticastIn4Address()).c_str(), socket.getLocalInterfaceAddress().c_str());
            int nbytes = socket.send(udp_packet, sizeof(udp_packet));
#endif
            if (nbytes != sizeof(udp_packet)) {
                logger.print(LogLevel::LOG_ERROR, "cannot send udp packet %d\n", nbytes);
            }
        }

        // sleep for 1000 milliseconds
        LocalHost::sleep(1000);
    }

    return 0;
}


// insert obis data into the given emeter packet
void* insert(SpeedwireEmeterProtocol& emeter_packet, void* const obis, const ObisData& obis_data, const double value) {
    // create a new obis data instance from the given obis data template instance
    ObisData temp(obis_data);
    // set its measurement value
    temp.measurementValues.addMeasurement(value, 0);
    // convert it into the obis byte representation
    std::array<uint8_t, 12> byte_array = temp.toByteArray();
    // insert it into the given emeter packet 
    return emeter_packet.setObisElement(obis, byte_array.data());
}

// insert obis data into the given emeter packet
void* insert(SpeedwireEmeterProtocol& emeter_packet, void* const obis, const ObisData& obis_data, const std::string& value) {
    // create a new obis data instance from the given obis data template instance
    ObisData temp(obis_data);
    // set its measurement value
    temp.measurementValues.value_string = value;
    // convert it into the obis byte representation
    std::array<uint8_t, 12> byte_array = temp.toByteArray();
    // insert it into the given emeter packet 
    return emeter_packet.setObisElement(obis, byte_array.data());
}