import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(const Esp32CamApp());
}

class Esp32CamApp extends StatelessWidget {
  const Esp32CamApp({super.key});

  @override
  Widget build(BuildContext context) {
    const teal = Color(0xFF00897B);
    const amber = Color(0xFFF9A825);

    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'ESP32-CAM BT',
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
          seedColor: teal,
          primary: teal,
          secondary: amber,
          surface: Colors.white,
        ),
        scaffoldBackgroundColor: const Color(0xFFF5F8F7),
        appBarTheme: const AppBarTheme(
          backgroundColor: Colors.white,
          foregroundColor: Color(0xFF17211F),
          elevation: 0,
          centerTitle: false,
        ),
      ),
      home: const HomePage(),
    );
  }
}

class EspDevice {
  const EspDevice({
    required this.name,
    required this.address,
    required this.bonded,
  });

  final String name;
  final String address;
  final bool bonded;

  factory EspDevice.fromEvent(Map<String, dynamic> event) {
    return EspDevice(
      name: (event['name'] as String?)?.trim().isNotEmpty == true
          ? event['name'] as String
          : 'Dispositivo sem nome',
      address: event['address'] as String? ?? '',
      bonded: event['bonded'] as bool? ?? false,
    );
  }
}

class BluetoothBridge {
  static const MethodChannel _methods = MethodChannel('esp32cam_bt/methods');
  static const EventChannel _events = EventChannel('esp32cam_bt/events');

  Stream<Map<String, dynamic>> get events {
    return _events.receiveBroadcastStream().map((event) {
      final raw = Map<Object?, Object?>.from(event as Map);
      return <String, dynamic>{
        for (final entry in raw.entries) entry.key.toString(): entry.value,
      };
    });
  }

  Future<bool> requestPermissions() async {
    return await _methods.invokeMethod<bool>('requestPermissions') ?? false;
  }

  Future<bool> startDiscovery() async {
    return await _methods.invokeMethod<bool>('startDiscovery') ?? false;
  }

  Future<void> stopDiscovery() async {
    await _methods.invokeMethod<void>('stopDiscovery');
  }

  Future<bool> connect(String address) async {
    return await _methods.invokeMethod<bool>('connect', {'address': address}) ??
        false;
  }

  Future<bool> sendCommand(String command) async {
    return await _methods.invokeMethod<bool>('sendCommand', {
          'command': command,
        }) ??
        false;
  }

  Future<void> disconnect() async {
    await _methods.invokeMethod<void>('disconnect');
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final BluetoothBridge _bridge = BluetoothBridge();
  final Map<String, EspDevice> _devices = {};

  StreamSubscription<Map<String, dynamic>>? _subscription;
  Uint8List? _latestImage;
  String _status = 'Pronto';
  String? _connectedAddress;
  String? _connectingAddress;
  String? _lastSavedPath;
  int _frameCount = 0;
  bool _scanning = false;
  bool _connected = false;
  bool _capturing = false;

  @override
  void initState() {
    super.initState();
    _subscription = _bridge.events.listen(
      _handleEvent,
      onError: (error) => _showError('Canal Bluetooth: $error'),
    );
  }

  @override
  void dispose() {
    _subscription?.cancel();
    _bridge.disconnect();
    super.dispose();
  }

  void _handleEvent(Map<String, dynamic> event) {
    if (!mounted) {
      return;
    }

    final type = event['type'] as String?;

    switch (type) {
      case 'device':
        final device = EspDevice.fromEvent(event);
        if (device.address.isNotEmpty) {
          setState(() {
            _devices[device.address] = device;
          });
        }
        break;
      case 'status':
        final message = event['message'] as String? ?? 'Status atualizado';
        final connected = event['connected'] as bool?;
        final scanning = event['scanning'] as bool?;
        setState(() {
          _status = message;
          if (connected != null) {
            _connected = connected;
            if (!connected) {
              _connectedAddress = null;
              _connectingAddress = null;
            }
          }
          if (scanning != null) {
            _scanning = scanning;
          }
        });
        break;
      case 'frame':
        final bytes = event['bytes'];
        if (bytes is Uint8List) {
          setState(() {
            _latestImage = bytes;
            _frameCount++;
            _capturing = false;
            _status = event['kind'] == 'PHOTO'
                ? 'Foto recebida'
                : 'Frame recebido';
          });
        }
        break;
      case 'saved':
        final path = event['path'] as String? ?? 'microSD';
        setState(() {
          _lastSavedPath = path;
          _capturing = false;
          _status = 'Foto salva no cartao SD';
        });
        _showSnack('Foto salva: $path');
        break;
      case 'error':
        _showError(event['message'] as String? ?? 'Erro desconhecido');
        break;
    }
  }

  Future<void> _findDevices() async {
    final granted = await _bridge.requestPermissions();
    if (!mounted) {
      return;
    }

    if (!granted) {
      _showError('Permissoes Bluetooth negadas');
      return;
    }

    setState(() {
      _devices.clear();
      _scanning = true;
      _status = 'Procurando dispositivos';
    });

    final started = await _bridge.startDiscovery();
    if (!mounted) {
      return;
    }

    if (!started) {
      setState(() {
        _scanning = false;
      });
      _showError('Nao foi possivel iniciar a busca');
    }
  }

  Future<void> _connect(EspDevice device) async {
    final granted = await _bridge.requestPermissions();
    if (!mounted) {
      return;
    }

    if (!granted) {
      _showError('Permissoes Bluetooth negadas');
      return;
    }

    setState(() {
      _connectingAddress = device.address;
      _status = 'Conectando';
    });

    await _bridge.stopDiscovery();
    final ok = await _bridge.connect(device.address);
    if (!mounted) {
      return;
    }

    if (!ok) {
      setState(() {
        _connectingAddress = null;
      });
      _showError('Falha ao conectar');
      return;
    }

    setState(() {
      _connected = true;
      _connectedAddress = device.address;
      _connectingAddress = null;
      _status = 'Conectado';
    });

    await _bridge.sendCommand('SET_INTERVAL 1500');
    await _bridge.sendCommand('START_STREAM');
  }

  Future<void> _disconnect() async {
    await _bridge.sendCommand('STOP_STREAM');
    await _bridge.disconnect();
    if (!mounted) {
      return;
    }

    setState(() {
      _connected = false;
      _connectedAddress = null;
      _connectingAddress = null;
      _status = 'Desconectado';
    });
  }

  Future<void> _takePhoto({required bool flash}) async {
    if (!_connected || _capturing) {
      return;
    }

    setState(() {
      _capturing = true;
      _status = flash ? 'Capturando com flash' : 'Capturando sem flash';
    });

    final ok = await _bridge.sendCommand(flash ? 'CAPTURE 1' : 'CAPTURE 0');
    if (!mounted) {
      return;
    }

    if (!ok) {
      setState(() {
        _capturing = false;
      });
      _showError('Nao foi possivel enviar o comando');
    }
  }

  void _showError(String message) {
    if (!mounted) {
      return;
    }
    setState(() {
      _status = message;
      _scanning = false;
      _connectingAddress = null;
      _capturing = false;
    });
    _showSnack(message);
  }

  void _showSnack(String message) {
    if (!mounted) {
      return;
    }
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(message),
        behavior: SnackBarBehavior.floating,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('ESP32-CAM Bluetooth'),
        actions: [
          if (_connected)
            Tooltip(
              message: 'Desconectar',
              child: IconButton(
                icon: const Icon(Icons.bluetooth_disabled),
                onPressed: _disconnect,
              ),
            ),
        ],
      ),
      body: SafeArea(
        child: LayoutBuilder(
          builder: (context, constraints) {
            final wide = constraints.maxWidth >= 760;
            final controls = _buildControls(context);
            final preview = _buildPreview(context);

            return SingleChildScrollView(
              padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
              child: wide
                  ? Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Expanded(child: controls),
                        const SizedBox(width: 16),
                        Expanded(child: preview),
                      ],
                    )
                  : Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        controls,
                        const SizedBox(height: 16),
                        preview,
                      ],
                    ),
            );
          },
        ),
      ),
    );
  }

  Widget _buildControls(BuildContext context) {
    final devices = _devices.values.toList()
      ..sort((a, b) {
        final aIsEsp = a.name.toUpperCase().contains('ESP32');
        final bIsEsp = b.name.toUpperCase().contains('ESP32');
        if (aIsEsp != bIsEsp) {
          return aIsEsp ? -1 : 1;
        }
        return a.name.compareTo(b.name);
      });

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        StatusStrip(
          connected: _connected,
          scanning: _scanning,
          text: _status,
        ),
        const SizedBox(height: 14),
        FilledButton.icon(
          onPressed: _scanning ? null : _findDevices,
          icon: Icon(_scanning ? Icons.radar : Icons.bluetooth_searching),
          label: Text(_scanning ? 'Localizando...' : 'Localizar ESP32-CAM'),
          style: FilledButton.styleFrom(
            minimumSize: const Size.fromHeight(52),
          ),
        ),
        const SizedBox(height: 18),
        Row(
          children: [
            const Icon(Icons.devices, size: 20),
            const SizedBox(width: 8),
            Text(
              'Dispositivos encontrados',
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                  ),
            ),
          ],
        ),
        const SizedBox(height: 10),
        if (devices.isEmpty)
          EmptyDeviceList(scanning: _scanning)
        else
          ...devices.map(
            (device) => Padding(
              padding: const EdgeInsets.only(bottom: 10),
              child: DeviceTile(
                device: device,
                connected: _connectedAddress == device.address,
                connecting: _connectingAddress == device.address,
                onConnect: _connected ? null : () => _connect(device),
              ),
            ),
          ),
      ],
    );
  }

  Widget _buildPreview(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            const Icon(Icons.photo_camera, size: 20),
            const SizedBox(width: 8),
            Text(
              'Camera',
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w700,
                  ),
            ),
            const Spacer(),
            Text(
              '$_frameCount frames',
              style: Theme.of(context).textTheme.labelLarge?.copyWith(
                    color: Colors.black54,
                  ),
            ),
          ],
        ),
        const SizedBox(height: 10),
        CameraPreviewBox(
          bytes: _latestImage,
          connected: _connected,
          capturing: _capturing,
        ),
        if (_lastSavedPath != null) ...[
          const SizedBox(height: 10),
          SavedPhotoStrip(path: _lastSavedPath!),
        ],
        const SizedBox(height: 14),
        Wrap(
          spacing: 10,
          runSpacing: 10,
          children: [
            ActionButton(
              label: 'Tirar foto sem flash',
              icon: Icons.flash_off,
              enabled: _connected && !_capturing,
              onPressed: () => _takePhoto(flash: false),
            ),
            ActionButton(
              label: 'Tirar foto com flash',
              icon: Icons.flash_on,
              enabled: _connected && !_capturing,
              tonal: true,
              onPressed: () => _takePhoto(flash: true),
            ),
          ],
        ),
      ],
    );
  }
}

class StatusStrip extends StatelessWidget {
  const StatusStrip({
    super.key,
    required this.connected,
    required this.scanning,
    required this.text,
  });

  final bool connected;
  final bool scanning;
  final String text;

  @override
  Widget build(BuildContext context) {
    final color = connected
        ? const Color(0xFF00796B)
        : scanning
            ? const Color(0xFF6D4C41)
            : const Color(0xFF455A64);

    return DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: const Color(0xFFE0E6E4)),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
        child: Row(
          children: [
            Icon(
              connected ? Icons.bluetooth_connected : Icons.info_outline,
              color: color,
              size: 21,
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Text(
                text,
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                      fontWeight: FontWeight.w600,
                    ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class EmptyDeviceList extends StatelessWidget {
  const EmptyDeviceList({super.key, required this.scanning});

  final bool scanning;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: const Color(0xFFE0E6E4)),
      ),
      child: Padding(
        padding: const EdgeInsets.all(18),
        child: Row(
          children: [
            Icon(
              scanning ? Icons.search : Icons.bluetooth,
              color: Colors.black45,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(
                scanning
                    ? 'Busca em andamento'
                    : 'Nenhum dispositivo listado',
                style: Theme.of(context).textTheme.bodyMedium,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class DeviceTile extends StatelessWidget {
  const DeviceTile({
    super.key,
    required this.device,
    required this.connected,
    required this.connecting,
    required this.onConnect,
  });

  final EspDevice device;
  final bool connected;
  final bool connecting;
  final VoidCallback? onConnect;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.white,
      borderRadius: BorderRadius.circular(8),
      child: InkWell(
        borderRadius: BorderRadius.circular(8),
        onTap: onConnect,
        child: Container(
          padding: const EdgeInsets.all(14),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(8),
            border: Border.all(color: const Color(0xFFE0E6E4)),
          ),
          child: Row(
            children: [
              CircleAvatar(
                radius: 20,
                backgroundColor: connected
                    ? const Color(0xFFE0F2F1)
                    : const Color(0xFFF1F4F3),
                foregroundColor: connected
                    ? const Color(0xFF00796B)
                    : const Color(0xFF455A64),
                child: Icon(connected ? Icons.check : Icons.memory, size: 20),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      device.name,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.titleSmall?.copyWith(
                            fontWeight: FontWeight.w700,
                          ),
                    ),
                    const SizedBox(height: 3),
                    Text(
                      '${device.address}${device.bonded ? '  Pareado' : ''}',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                            color: Colors.black54,
                          ),
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 10),
              if (connecting)
                const SizedBox(
                  width: 22,
                  height: 22,
                  child: CircularProgressIndicator(strokeWidth: 2.4),
                )
              else
                TextButton.icon(
                  onPressed: onConnect,
                  icon: Icon(connected ? Icons.done : Icons.link),
                  label: Text(connected ? 'Conectado' : 'Conectar'),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class CameraPreviewBox extends StatelessWidget {
  const CameraPreviewBox({
    super.key,
    required this.bytes,
    required this.connected,
    required this.capturing,
  });

  final Uint8List? bytes;
  final bool connected;
  final bool capturing;

  @override
  Widget build(BuildContext context) {
    return AspectRatio(
      aspectRatio: 4 / 3,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: const Color(0xFF111816),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: const Color(0xFF263531), width: 1),
        ),
        child: ClipRRect(
          borderRadius: BorderRadius.circular(7),
          child: Stack(
            fit: StackFit.expand,
            children: [
              if (bytes != null)
                Image.memory(
                  bytes!,
                  fit: BoxFit.contain,
                  gaplessPlayback: true,
                )
              else
                Center(
                  child: Icon(
                    connected ? Icons.photo_camera : Icons.bluetooth,
                    color: Colors.white54,
                    size: 58,
                  ),
                ),
              if (capturing)
                Container(
                  color: Colors.black38,
                  child: const Center(
                    child: CircularProgressIndicator(color: Colors.white),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class SavedPhotoStrip extends StatelessWidget {
  const SavedPhotoStrip({super.key, required this.path});

  final String path;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xFFFFF8E1),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: const Color(0xFFFFECB3)),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
        child: Row(
          children: [
            const Icon(Icons.sd_card, color: Color(0xFF8D6E00), size: 20),
            const SizedBox(width: 8),
            Expanded(
              child: Text(
                'Foto salva no cartao SD: $path',
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class ActionButton extends StatelessWidget {
  const ActionButton({
    super.key,
    required this.label,
    required this.icon,
    required this.enabled,
    required this.onPressed,
    this.tonal = false,
  });

  final String label;
  final IconData icon;
  final bool enabled;
  final VoidCallback onPressed;
  final bool tonal;

  @override
  Widget build(BuildContext context) {
    final button = tonal
        ? FilledButton.tonalIcon(
            onPressed: enabled ? onPressed : null,
            icon: Icon(icon),
            label: Text(label),
          )
        : FilledButton.icon(
            onPressed: enabled ? onPressed : null,
            icon: Icon(icon),
            label: Text(label),
          );

    return ConstrainedBox(
      constraints: const BoxConstraints(minWidth: 210, minHeight: 48),
      child: button,
    );
  }
}
