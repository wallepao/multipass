/*
 * Copyright (C) 2017-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <multipass/constants.h>
#include <multipass/exceptions/autostart_setup_exception.h>
#include <multipass/exceptions/not_implemented_on_this_backend_exception.h>
#include <multipass/exceptions/settings_exceptions.h>
#include <multipass/exceptions/snap_environment_exception.h>
#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/platform.h>
#include <multipass/process/qemuimg_process_spec.h>
#include <multipass/process/simple_process_spec.h>
#include <multipass/snap_utils.h>
#include <multipass/standard_paths.h>
#include <multipass/utils.h>
#include <multipass/virtual_machine_factory.h>

#include "backends/libvirt/libvirt_virtual_machine_factory.h"
#include "backends/lxd/lxd_virtual_machine_factory.h"
#include "backends/qemu/qemu_virtual_machine_factory.h"
#include "logger/journald_logger.h"
#include "platform_linux.h"
#include "platform_shared.h"
#include "shared/linux/process_factory.h"
#include "shared/sshfs_server_process_spec.h"
#include <disabled_update_prompt.h>

#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QRegularExpression>
#include <QTextStream>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mu = multipass::utils;

namespace
{
constexpr auto autostart_filename = "multipass.gui.autostart.desktop";
constexpr auto category = "linux platform";

// The uevent files in /sys have a set of KEY=value pairs, one per line. This function takes as arguments a file
// with full path and a key, and returns the value associated with that key. If the specified uevent file does not
// exist, or the key is not found, then the empty string is returned.
std::string get_uevent_value(const std::string& file, const std::string& key)
{
    QRegularExpression pattern('^' + QString::fromStdString(key) + "=(?<value>[A-Za-z0-9-_]*)$");
    QFile uevent_file(QString::fromStdString(file));

    if (uevent_file.open(QIODevice::ReadOnly))
    {
        while (!uevent_file.atEnd())
        {
            auto key_match = pattern.match(uevent_file.readLine());
            if (key_match.hasMatch())
            {
                uevent_file.close();
                return key_match.captured("value").toStdString();
            }
        }
        uevent_file.close();
    }

    return std::string();
}

QString get_ip_output(QStringList ip_args)
{
    auto ip_spec = mp::simple_process_spec("ip", ip_args);
    auto ip_process = mp::platform::make_process(std::move(ip_spec));
    auto ip_exit_state = ip_process->execute();

    if (ip_exit_state.completed_successfully())
        return ip_process->read_all_standard_output();
    else if (ip_exit_state.exit_code && *(ip_exit_state.exit_code) == 1)
        return ip_process->read_all_standard_error();
    else
        throw std::runtime_error(fmt::format("Failed to execute ip: {}", ip_process->read_all_standard_error()));
}

QSet<QString> get_wireless_devices(const std::string& physical_path)
{
    QSet<QString> wifi_devices;

    QFile wifi_file(QString::fromStdString(physical_path + "wireless"));
    if (!wifi_file.open(QIODevice::ReadOnly | QIODevice::Text))
        return wifi_devices;

    auto device_regex = QRegularExpression(QStringLiteral("^(?<name>\\w+):.*$"), QRegularExpression::MultilineOption);

    QTextStream in(&wifi_file);
    while (!in.atEnd())
    {
        auto device_match = device_regex.match(in.readLine());
        if (device_match.hasMatch())
            wifi_devices << device_match.captured("name");
    }

    wifi_file.close();

    return wifi_devices;
}

mp::NetworkInterfaceInfo get_network_interface_info(const std::string& iface_name)
{
    std::string virtual_path("/sys/devices/virtual/net/" + iface_name);
    std::string physical_path("/proc/net/");

    // The interface can be a hardware or virtual one. To distinguish between them, we see if a folder with the
    // interface name exists in /sys/devices/net/virtual.
    if (QFile::exists(QString::fromStdString(virtual_path)))
    {
        return mp::platform::get_virtual_interface_info(iface_name, virtual_path);
    }
    else
    {
        return mp::platform::get_physical_interface_info(iface_name, physical_path);
    }
}

} // namespace

std::map<QString, QString> mp::platform::extra_settings_defaults()
{
    return {};
}

QString mp::platform::interpret_setting(const QString& key, const QString& val)
{
    if (key == hotkey_key)
        return mp::platform::interpret_hotkey(val);

    // this should not happen (settings should have found it to be an invalid key)
    throw InvalidSettingsException(key, val, "Setting unavailable on Linux");
}

void mp::platform::sync_winterm_profiles()
{
    // NOOP on Linux
}

QString mp::platform::autostart_test_data()
{
    return autostart_filename;
}

void mp::platform::setup_gui_autostart_prerequisites()
{
    const auto config_dir = QDir{MP_STDPATHS.writableLocation(StandardPaths::GenericConfigLocation)};
    const auto link_dir = QDir{config_dir.absoluteFilePath("autostart")};
    mu::link_autostart_file(link_dir, mp::client_name, autostart_filename);
}

std::string mp::platform::default_server_address()
{
    std::string base_dir;

    try
    {
        // if Snap, client and daemon can both access $SNAP_COMMON so can put socket there
        base_dir = mu::snap_common_dir().toStdString();
    }
    catch (const mp::SnapEnvironmentException&)
    {
        base_dir = "/run";
    }
    return "unix:" + base_dir + "/multipass_socket";
}

QString mp::platform::default_driver()
{
    return QStringLiteral("qemu");
}

QString mp::platform::daemon_config_home() // temporary
{
    auto ret = QString{qgetenv("DAEMON_CONFIG_HOME")};
    if (ret.isEmpty())
        ret = QStringLiteral("/root/.config");

    ret = QDir{ret}.absoluteFilePath(mp::daemon_name);
    return ret;
}

bool mp::platform::is_backend_supported(const QString& backend)
{
    return backend == "qemu" || backend == "libvirt" || backend == "lxd";
}

mp::VirtualMachineFactory::UPtr mp::platform::vm_backend(const mp::Path& data_dir)
{
    const auto& driver = utils::get_driver_str();
    if (driver == QStringLiteral("qemu"))
        return std::make_unique<QemuVirtualMachineFactory>(data_dir);
    else if (driver == QStringLiteral("libvirt"))
        return std::make_unique<LibVirtVirtualMachineFactory>(data_dir);
    else if (driver == QStringLiteral("lxd"))
        return std::make_unique<LXDVirtualMachineFactory>(data_dir);
    else
        throw std::runtime_error(fmt::format("Unsupported virtualization driver: {}", driver));
}

std::unique_ptr<mp::Process> mp::platform::make_sshfs_server_process(const mp::SSHFSServerConfig& config)
{
    return MP_PROCFACTORY.create_process(std::make_unique<mp::SSHFSServerProcessSpec>(config));
}

std::unique_ptr<mp::Process> mp::platform::make_process(std::unique_ptr<mp::ProcessSpec>&& process_spec)
{
    return MP_PROCFACTORY.create_process(std::move(process_spec));
}

mp::UpdatePrompt::UPtr mp::platform::make_update_prompt()
{
    return std::make_unique<DisabledUpdatePrompt>();
}

mp::logging::Logger::UPtr mp::platform::make_logger(mp::logging::Level level)
{
    return std::make_unique<logging::JournaldLogger>(level);
}

bool mp::platform::link(const char* target, const char* link)
{
    return ::link(target, link) == 0;
}

bool mp::platform::is_alias_supported(const std::string& alias, const std::string& remote)
{
    return true;
}

bool mp::platform::is_remote_supported(const std::string& remote)
{
    auto driver = utils::get_driver_str();

    // snapcraft:core{18,20} images don't work on LXD yet, so whack it altogether.
    if (driver == "lxd" && remote == "snapcraft")
        return false;

    return true;
}

bool mp::platform::is_image_url_supported()
{
    return true;
}

// This function is not working under confinement. Multipass has `network-control`, which allows rw access to
// `/sys/devices/virtual/net/*/bridge/*`.
mp::NetworkInterfaceInfo mp::platform::get_virtual_interface_info(const std::string& iface_name,
                                                                  const std::string& virtual_path)
{
    std::string type;
    std::string description;

    QString iface_dir_name = QString::fromStdString(virtual_path);

    // Only bridges contain a directory named `bridge` and one named `brif` in virtual_path.
    if (QFile::exists(iface_dir_name + "/bridge"))
    {
        type = "bridge";
        QDir bridged_dir(iface_dir_name + "/brif");
        QStringList all_bridged = bridged_dir.entryList(QDir::NoDotAndDotDot | QDir::Dirs);

        description += all_bridged.isEmpty() ? "Empty network bridge"
                                             : fmt::format("Network bridge with {}", all_bridged.join(", "));

        return mp::NetworkInterfaceInfo{iface_name, type, description};
    }

    // If the virtual interface is not a bridge, then it is TUN or TAP when the file `tun_flags` exists in
    // virtual_path.
    if (QFile::exists(iface_dir_name + "/tun_flags"))
    {
        // Only the TAP devices have a directory named `brport` in virtual_path.
        if (QFile::exists(iface_dir_name + "/brport"))
        {
            type = "tap";
            description = "TAP virtual interface";
        }
        else
        {
            type = "tun";
            description = "TUN virtual interface";
        }
    }
    else
    {
        type = "virtual";
        description = "Virtual interface";
    }

    auto associated_to = get_uevent_value((iface_dir_name + "/brport/bridge/uevent").toStdString(), "INTERFACE");
    if (!associated_to.empty())
        description += " associated to " + associated_to;

    return mp::NetworkInterfaceInfo{iface_name, type, description};
}

mp::NetworkInterfaceInfo mp::platform::get_physical_interface_info(const std::string& iface_name,
                                                                   const std::string& physical_path)
{
    if (get_wireless_devices(physical_path).contains(QString::fromStdString(iface_name)))
        return mp::NetworkInterfaceInfo{iface_name, "wifi", "Wi-Fi device"};
    else
        return mp::NetworkInterfaceInfo{iface_name, "ethernet", "Ethernet device"};
}

std::map<std::string, mp::NetworkInterfaceInfo> mp::platform::get_network_interfaces_info()
{
    auto ifaces_info = std::map<std::string, mp::NetworkInterfaceInfo>();

    // All interfaces will be returned by the command ip.
    auto ip_output = get_ip_output({"address"});

    mpl::log(mpl::Level::trace, category, fmt::format("Got the following output from ip:\n{}", ip_output));

    const auto pattern = QStringLiteral("^\\d+: (?<name>[A-Za-z0-9-_]+): .*$");
    const auto regexp = QRegularExpression{pattern, QRegularExpression::MultilineOption};
    QRegularExpressionMatchIterator match_it = regexp.globalMatch(ip_output);

    while (match_it.hasNext())
    {
        auto match = match_it.next();
        if (match.hasMatch())
        {
            std::string iface_name = match.captured("name").toStdString();
            ifaces_info.emplace(iface_name, get_network_interface_info(iface_name));
        }
    }

    return ifaces_info;
}

std::string mp::platform::reinterpret_interface_id(const std::string& ux_id)
{
    return ux_id;
}
