import 'package:flutter/material.dart';
import 'package:micnote_core/micnote_core.dart';

void main() {
  runApp(const MicnoteApp());
}

class MicnoteApp extends StatelessWidget {
  const MicnoteApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Micnote',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
      ),
      home: const HomePage(),
    );
  }
}

/// Stage 0b's only screen: proves the app reaches libmicnote through FFI.
class HomePage extends StatelessWidget {
  const HomePage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Micnote')),
      body: Center(child: Text('libmicnote ${coreVersion()}')),
    );
  }
}
