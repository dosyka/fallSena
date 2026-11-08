import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

import '../home/home_screen.dart';
import '../services/auth_service.dart';
import 'auth_screen.dart';

/// Routes between the signed-out auth experience and the signed-in home,
/// driven by [FirebaseAuth.authStateChanges].
class AuthGate extends StatelessWidget {
  const AuthGate({super.key, this.authService});

  final AuthService? authService;

  @override
  Widget build(BuildContext context) {
    final service = authService ?? AuthService();
    return StreamBuilder<User?>(
      stream: service.authStateChanges(),
      builder: (context, snapshot) {
        if (snapshot.connectionState == ConnectionState.waiting) {
          return const Scaffold(
            body: Center(child: CircularProgressIndicator()),
          );
        }
        if (snapshot.hasData) {
          return HomeScreen(authService: service, user: snapshot.data!);
        }
        return AuthScreen(authService: service);
      },
    );
  }
}
