import sys

from PySide6.QtWidgets import QApplication

from rcp2027_app.ui.main_window import MainWindow


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("RCP2027")
    app.setOrganizationName("RCP2027")

    window = MainWindow()
    window.show()
    return app.exec()
