from __future__ import annotations

from pathlib import Path

from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QPlainTextEdit,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from rcp2027_app.data.schemas import (
    ATTENDANCE_FIELDS,
    ATTENDANCE_FILE,
    EMPLOYEE_FIELDS,
    EMPLOYEES_FILE,
    LOG_FIELDS,
    LOGS_FILE,
    DeviceConfig,
    apps_script_request,
    check_apps_script,
    generate_api_token,
    initialize_sd_csv_files,
    load_config,
    load_csv,
    load_csv_file,
    prepare_sd_card,
    save_config,
    save_csv_file,
    validate_attendance_rows,
    validate_apps_script_url,
    validate_employee_rows,
    validate_sheet_url,
)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("RCP2027 — Rejestrator czasu pracy")
        self.resize(1120, 760)
        self.folder: Path | None = None
        self.remote_revisions: dict[str, str] = {}
        self.csv_paths: dict[str, Path] = {}

        tabs = QTabWidget()
        tabs.addTab(self._configuration_page(), "Konfiguracja i karta SD")
        self.employee_table = self._table_page(
            tabs, EMPLOYEE_FIELDS, EMPLOYEES_FILE, "Pracownicy", self._save_employees,
            "employees",
        )
        self.attendance_table = self._table_page(
            tabs, ATTENDANCE_FIELDS, ATTENDANCE_FILE, "Ewidencja czasu",
            self._save_attendance, "attendanceRecords", select_csv=True,
        )
        self.log_table = self._table_page(
            tabs, LOG_FIELDS, LOGS_FILE, "Logi", self._save_logs,
            "logs", editable=False, select_csv=True,
        )
        self.setCentralWidget(tabs)
        self.setStyleSheet(
            """
            QMainWindow, QWidget { background: #f4f6fa; color: #172033; }
            QTabWidget::pane { border: 1px solid #dce2ec; background: white; }
            QTabBar::tab { padding: 12px 18px; background: #e9edf4; }
            QTabBar::tab:selected { background: #ffffff; color: #2457d6; }
            QLineEdit, QComboBox, QTableWidget {
                background: white; border: 1px solid #cbd3df; padding: 7px;
            }
            QPushButton { background: #2457d6; color: white; padding: 9px 14px;
                          border: 0; border-radius: 5px; }
            QPushButton:hover { background: #1745b8; }
            """
        )

    def _configuration_page(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        heading = QLabel("Konfiguracja terminala RCP2027")
        heading.setStyleSheet("font-size: 22px; font-weight: 700; padding: 12px;")
        layout.addWidget(heading)

        folder_row = QHBoxLayout()
        self.folder_label = QLabel("Nie wybrano głównego katalogu karty SD (np. E:\\)")
        browse = QPushButton("Wybierz kartę SD")
        browse.clicked.connect(self._choose_folder)
        folder_row.addWidget(self.folder_label, 1)
        folder_row.addWidget(browse)
        layout.addLayout(folder_row)

        form = QFormLayout()
        self.mode = QComboBox()
        self.mode.addItem("Offline — karta SD", "offline")
        self.mode.addItem("Online — Arkusze Google", "online")
        self.language = QComboBox()
        for label, code in (
            ("Polski", "pl"),
            ("English", "en"),
            ("Español", "es"),
            ("Deutsch", "de"),
        ):
            self.language.addItem(label, code)
        self.ssid = QLineEdit()
        self.wifi_password = QLineEdit()
        self.wifi_password.setEchoMode(QLineEdit.EchoMode.Password)
        self.timezone_offset = QLineEdit("60")
        self.timezone_offset.setPlaceholderText("np. 60 dla UTC+01:00")
        self.apps_script = QLineEdit()
        self.api_token = QLineEdit()
        self.api_token.setEchoMode(QLineEdit.EchoMode.Password)
        token_row = QHBoxLayout()
        token_row.addWidget(self.api_token, 1)
        generate_token = QPushButton("Generuj token")
        generate_token.clicked.connect(lambda: self.api_token.setText(generate_api_token()))
        token_row.addWidget(generate_token)
        self.employees_sheet = QLineEdit()
        self.attendance_sheet = QLineEdit()
        self.logs_sheet = QLineEdit()
        self.camera_enabled = QCheckBox("Włącz wykonywanie zdjęć przy odbiciu")
        self.camera_enabled.setChecked(False)
        for label, widget in (
            ("Tryb pracy", self.mode),
            ("Język terminala", self.language),
            ("Nazwa sieci Wi-Fi", self.ssid),
            ("Hasło Wi-Fi", self.wifi_password),
            ("Przesunięcie strefy czasu (minuty)", self.timezone_offset),
            ("Google Apps Script URL (/exec)", self.apps_script),
            ("Arkusz pracowników URL", self.employees_sheet),
            ("Arkusz wejść/wyjść URL", self.attendance_sheet),
            ("Arkusz logów URL", self.logs_sheet),
        ):
            form.addRow(label, widget)
        form.addRow("Token API (min. 32 znaki)", token_row)
        form.addRow("", self.camera_enabled)
        layout.addLayout(form)

        self.status = QLabel(
            "Linki do arkuszy służą do identyfikacji i kontroli. Odczyt/zapis terminala "
            "online realizuje wdrożone API Google Apps Script."
        )
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        buttons = QHBoxLayout()
        verify_links = QPushButton("Sprawdź linki")
        verify_links.clicked.connect(self._verify_links)
        test_api = QPushButton("Testuj API")
        test_api.clicked.connect(self._test_api)
        save = QPushButton("Zapisz konfigurację")
        save.clicked.connect(self._save_configuration)
        prepare = QPushButton("Przygotuj kartę SD")
        prepare.clicked.connect(self._prepare_sd_card)
        help_button = QPushButton("Pomoc")
        help_button.clicked.connect(self._show_help)
        buttons.addWidget(verify_links)
        buttons.addWidget(test_api)
        buttons.addWidget(help_button)
        buttons.addStretch()
        buttons.addWidget(save)
        buttons.addWidget(prepare)
        layout.addLayout(buttons)
        layout.addStretch()
        return page

    def _table_page(
        self,
        tabs: QTabWidget,
        fields: tuple[str, ...],
        filename: str,
        title: str,
        save_handler,
        remote_action: str,
        editable: bool = True,
        select_csv: bool = False,
    ) -> QTableWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        table = QTableWidget(0, len(fields))
        table.setHorizontalHeaderLabels(fields)
        table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        table.setAlternatingRowColors(True)
        layout.addWidget(table)
        controls = QHBoxLayout()
        if select_csv:
            open_file = QPushButton("Wybierz plik z karty SD")
            open_file.clicked.connect(
                lambda: self._choose_csv_file(table, fields, filename)
            )
            controls.addWidget(open_file)
            refresh = QPushButton("Odśwież z Google Sheets")
            refresh.clicked.connect(
                lambda: self._refresh_table(table, fields, filename, remote_action)
            )
            controls.addWidget(refresh)
        else:
            refresh = QPushButton("Odśwież")
            refresh.clicked.connect(
                lambda: self._refresh_table(table, fields, filename, remote_action)
            )
            controls.addWidget(refresh)
        if editable:
            add = QPushButton("Dodaj rekord")
            add.clicked.connect(lambda: table.insertRow(table.rowCount()))
            remove = QPushButton("Usuń zaznaczone")
            remove.clicked.connect(lambda: self._remove_selected(table))
            save = QPushButton("Zapisz / wyślij zmiany")
            save.clicked.connect(save_handler)
            controls.addWidget(add)
            controls.addWidget(remove)
            controls.addStretch()
            controls.addWidget(save)
        layout.addLayout(controls)
        tabs.addTab(page, title)
        return table

    def _refresh_table(
        self,
        table: QTableWidget,
        fields: tuple[str, ...],
        filename: str,
        remote_action: str,
    ) -> None:
        if self.mode.currentData() == "online":
            try:
                response = apps_script_request(
                    self.apps_script.text().strip(),
                    self.api_token.text(),
                    remote_action,
                )
                rows = response.get("rows")
                if not isinstance(rows, list):
                    raise ValueError("API nie zwróciło listy rekordów.")
                table.setRowCount(0)
                for row in rows:
                    if not isinstance(row, dict):
                        raise ValueError("API zwróciło rekord o nieprawidłowym formacie.")
                    row_index = table.rowCount()
                    table.insertRow(row_index)
                    for column, field in enumerate(fields):
                        value = row.get(field, "")
                        table.setItem(row_index, column, QTableWidgetItem(str(value)))
                revision = response.get("revision")
                if isinstance(revision, str):
                    self.remote_revisions[remote_action] = revision
            except (ValueError, OSError) as error:
                self._show_error(str(error))
                return
            self.status.setText(f"Pobrano dane z Google Apps Script: {filename}.")
            return
        self._load_table(table, fields, filename)

    def _choose_folder(self) -> None:
        selected = QFileDialog.getExistingDirectory(self, "Wybierz katalog karty SD")
        if not selected:
            return
        self.folder = Path(selected)
        self.remote_revisions.clear()
        self.csv_paths.clear()
        self.folder_label.setText(str(self.folder))
        try:
            created_csvs = initialize_sd_csv_files(self.folder)
            config = load_config(self.folder)
        except FileNotFoundError:
            self.status.setText("Utworzono brakujące pliki CSV z nagłówkami.")
            self._load_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)
            self.attendance_table.setRowCount(0)
            self.log_table.setRowCount(0)
        except (OSError, ValueError) as error:
            self._show_error(str(error))
        else:
            self._put_config(config)
            created_message = (
                f" Utworzono: {', '.join(path.name for path in created_csvs)}."
                if created_csvs
                else ""
            )
            self.status.setText(f"Wczytano config.json.{created_message}")
            if config.mode == "offline":
                self._load_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)
                self.attendance_table.setRowCount(0)
                self.log_table.setRowCount(0)
                self.status.setText(
                    "Konfigurację wczytano. Wskaż attendance.csv i logs.csv z karty SD."
                )
            else:
                self.employee_table.setRowCount(0)
                self.attendance_table.setRowCount(0)
                self.log_table.setRowCount(0)

    def _load_table(
        self, table: QTableWidget, fields: tuple[str, ...], filename: str
    ) -> None:
        if self.folder is None:
            return
        try:
            rows = load_csv(self.folder, filename, fields)
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        table.setRowCount(len(rows))
        for row_index, row in enumerate(rows):
            for column, field in enumerate(fields):
                table.setItem(row_index, column, QTableWidgetItem(row[field]))

    def _choose_csv_file(
        self, table: QTableWidget, fields: tuple[str, ...], filename: str
    ) -> None:
        if self.folder is None:
            self._show_error("Najpierw wybierz główny katalog karty SD.")
            return
        selected, _ = QFileDialog.getOpenFileName(
            self,
            f"Wybierz plik {filename} z karty SD",
            str(self.folder),
            "Pliki CSV (*.csv);;Wszystkie pliki (*)",
        )
        if not selected:
            return
        path = Path(selected)
        try:
            path.resolve().relative_to(self.folder.resolve())
        except ValueError:
            self._show_error("Wybierz plik znajdujący się w katalogu wybranej karty SD.")
            return
        try:
            rows = load_csv_file(path, fields)
            if path.stat().st_size == 0:
                raise ValueError(f"{path.name}: plik jest pusty i nie zawiera nagłówka CSV.")
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.csv_paths[filename] = path
        self._set_table_rows(table, fields, rows)
        self.status.setText(f"Wczytano {len(rows)} rekordów z {path}.")

    @staticmethod
    def _set_table_rows(
        table: QTableWidget, fields: tuple[str, ...], rows: list[dict[str, str]]
    ) -> None:
        table.setRowCount(len(rows))
        for row_index, row in enumerate(rows):
            for column, field in enumerate(fields):
                table.setItem(row_index, column, QTableWidgetItem(row[field]))

    @staticmethod
    def _remove_selected(table: QTableWidget) -> None:
        for row in sorted({index.row() for index in table.selectedIndexes()}, reverse=True):
            table.removeRow(row)

    def _table_rows(self, table: QTableWidget, fields: tuple[str, ...]) -> list[dict[str, str]]:
        rows = []
        for row_index in range(table.rowCount()):
            rows.append(
                {
                    field: (
                        table.item(row_index, column).text()
                        if table.item(row_index, column) is not None
                        else ""
                    )
                    for column, field in enumerate(fields)
                }
            )
        return rows

    def _save_table(
        self,
        table: QTableWidget,
        fields: tuple[str, ...],
        filename: str,
        path: Path | None = None,
    ) -> None:
        destination = path or (self.folder / filename if self.folder is not None else None)
        if destination is None:
            self._show_error("Najpierw wybierz katalog karty SD.")
            return
        try:
            save_csv_file(destination, fields, self._table_rows(table, fields))
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.status.setText(f"Zapisano plik {destination}.")

    def _save_remote_table(
        self, action: str, rows: list[dict[str, str]], filename: str
    ) -> None:
        answer = QMessageBox.warning(
            self,
            "Zastąp dane w Google Sheets",
            f"Zastąpić całą zawartość arkusza {filename} bieżącą tabelą? "
            "Ta operacja usunie wcześniejsze rekordy w tym arkuszu.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Cancel,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        read_action = {
            "replaceEmployees": "employees",
            "replaceAttendance": "attendanceRecords",
        }[action]
        revision = self.remote_revisions.get(read_action)
        if not revision:
            self._show_error("Najpierw odśwież dane z Google, aby uniknąć nadpisania nowych rekordów.")
            return
        try:
            response = apps_script_request(
                self.apps_script.text().strip(),
                self.api_token.text(),
                action,
                rows=rows,
                expected_revision=revision,
            )
        except (ValueError, OSError) as error:
            self._show_error(str(error))
            return
        new_revision = response.get("revision")
        if isinstance(new_revision, str):
            self.remote_revisions[read_action] = new_revision
        self.status.setText(f"Wysłano tabelę do Google Sheets: {filename}.")

    def _save_employees(self) -> None:
        rows = self._table_rows(self.employee_table, EMPLOYEE_FIELDS)
        try:
            validate_employee_rows(rows)
        except ValueError as error:
            self._show_error(str(error))
            return
        if self.mode.currentData() == "online":
            self._save_remote_table("replaceEmployees", rows, EMPLOYEES_FILE)
            return
        self._save_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)

    def _save_attendance(self) -> None:
        rows = self._table_rows(self.attendance_table, ATTENDANCE_FIELDS)
        try:
            validate_attendance_rows(rows)
        except ValueError as error:
            self._show_error(str(error))
            return
        if self.mode.currentData() == "online":
            self._save_remote_table("replaceAttendance", rows, ATTENDANCE_FILE)
            return
        path = self.csv_paths.get(ATTENDANCE_FILE)
        if path is None:
            self._show_error("Najpierw wybierz plik attendance.csv z karty SD.")
            return
        self._save_table(self.attendance_table, ATTENDANCE_FIELDS, ATTENDANCE_FILE, path)

    def _save_logs(self) -> None:
        self._show_error("Logi są tylko do odczytu.")

    def _show_help(self) -> None:
        script_path = Path(__file__).resolve().parents[4] / "google_apps_script" / "Code.gs"
        try:
            script = script_path.read_text(encoding="utf-8")
        except OSError as error:
            self._show_error(f"Nie można wczytać przykładowego kodu Apps Script: {error}")
            return

        dialog = QDialog(self)
        dialog.setWindowTitle("Pomoc — konfiguracja Google Sheets i Apps Script")
        dialog.resize(980, 780)
        layout = QVBoxLayout(dialog)
        instructions = QPlainTextEdit()
        instructions.setReadOnly(True)
        instructions.setPlainText(
            "1. Utwórz trzy osobne arkusze Google; w pierwszym wierszu pierwszej "
            "zakładki każdego arkusza wklej odpowiedni nagłówek dokładnie w tej "
            "kolejności:\\n\\n"
            "Pracownicy:\\n"
            "employee_id, card_uid, first_name, last_name, active, relay_access\\n\\n"
            "Ewidencja czasu:\\n"
            "event_id, timestamp, employee_id, card_uid, event_type, photo_file, "
            "source, notes\\n\\n"
            "Logi:\\n"
            "event_id, timestamp, level, code, message\\n\\n"
            "2. Otwórz https://script.google.com, utwórz projekt Apps Script i "
            "zastąp zawartość pliku Code.gs skopiowanym kodem poniżej. Zapisz "
            "projekt.\\n"
            "3. W ustawieniach projektu → Właściwości skryptu dodaj: "
            "RCP_API_TOKEN (ten sam tajny token co w aplikacji), "
            "EMPLOYEES_SHEET_ID, ATTENDANCE_SHEET_ID i LOGS_SHEET_ID. ID każdego "
            "arkusza to część adresu pomiędzy /spreadsheets/d/ a następnym /.\\n"
            "4. Wdróż → Nowe wdrożenie → Aplikacja internetowa. Wykonuj jako "
            "właściciel; dostęp ustaw zgodnie z polityką Google Workspace "
            "(urządzenie wymaga publicznego endpointu, a każda operacja jest "
            "chroniona tokenem). Skopiuj URL kończący się /exec.\\n"
            "5. W aplikacji wybierz tryb online, wklej URL /exec, token i linki "
            "do trzech arkuszy, kliknij Sprawdź linki oraz Testuj API, a potem "
            "Przygotuj kartę SD. Po zmianie kodu opublikuj nową wersję wdrożenia.\\n\\n"
            "Nie udostępniaj tokenu ani pliku config.json — zawiera token API i "
            "hasło Wi-Fi. Kod API:"
        )
        instructions.setMaximumHeight(270)
        layout.addWidget(instructions)
        code_box = QPlainTextEdit()
        code_box.setReadOnly(True)
        code_box.setPlainText(script)
        layout.addWidget(code_box, 1)
        controls = QHBoxLayout()
        copy_button = QPushButton("Kopiuj kod")
        copy_button.clicked.connect(
            lambda: QApplication.clipboard().setText(code_box.toPlainText())
        )
        close_button = QPushButton("Zamknij")
        close_button.clicked.connect(dialog.accept)
        controls.addWidget(copy_button)
        controls.addStretch()
        controls.addWidget(close_button)
        layout.addLayout(controls)
        dialog.exec()

    def _configuration(self) -> DeviceConfig:
        return DeviceConfig(
            mode=self.mode.currentData(),
            language=self.language.currentData(),
            wifi_ssid=self.ssid.text().strip(),
            wifi_password=self.wifi_password.text(),
            timezone_offset_minutes=int(self.timezone_offset.text()),
            apps_script_url=self.apps_script.text().strip(),
            api_token=self.api_token.text(),
            employees_sheet_url=self.employees_sheet.text().strip(),
            attendance_sheet_url=self.attendance_sheet.text().strip(),
            logs_sheet_url=self.logs_sheet.text().strip(),
            camera_enabled=self.camera_enabled.isChecked(),
        )

    def _put_config(self, config: DeviceConfig) -> None:
        for widget, value in ((self.mode, config.mode), (self.language, config.language)):
            index = widget.findData(value)
            if index >= 0:
                widget.setCurrentIndex(index)
        self.ssid.setText(config.wifi_ssid)
        self.wifi_password.setText(config.wifi_password)
        self.timezone_offset.setText(str(config.timezone_offset_minutes))
        self.apps_script.setText(config.apps_script_url)
        self.api_token.setText(config.api_token)
        self.employees_sheet.setText(config.employees_sheet_url)
        self.attendance_sheet.setText(config.attendance_sheet_url)
        self.logs_sheet.setText(config.logs_sheet_url)
        self.camera_enabled.setChecked(config.camera_enabled)

    def _verify_links(self) -> None:
        try:
            apps_script = self.apps_script.text().strip()
            if apps_script:
                validate_apps_script_url(apps_script)
            sheet_ids = []
            for field in (
                self.employees_sheet,
                self.attendance_sheet,
                self.logs_sheet,
            ):
                value = field.text().strip()
                if value:
                    sheet_ids.append(validate_sheet_url(value))
            if not apps_script and not sheet_ids:
                raise ValueError("Wklej co najmniej jeden link Google.")
        except ValueError as error:
            self.status.setText(f"Błąd linku: {error}")
            return
        self.status.setText(
            f"Format linków jest prawidłowy. Rozpoznano {len(sheet_ids)} arkuszy."
        )

    def _test_api(self) -> None:
        try:
            result = check_apps_script(
                self.apps_script.text().strip(),
                self.api_token.text(),
                (
                    self.employees_sheet.text().strip(),
                    self.attendance_sheet.text().strip(),
                    self.logs_sheet.text().strip(),
                ),
            )
        except ValueError as error:
            self.status.setText(str(error))
            return
        self.status.setText(result)

    def _save_configuration(self) -> None:
        if self.folder is None:
            self._show_error("Najpierw wybierz katalog karty SD.")
            return
        try:
            path = save_config(self.folder, self._configuration())
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.status.setText(f"Konfigurację zapisano do {path}.")

    def _prepare_sd_card(self) -> None:
        if self.folder is None:
            self._show_error("Najpierw wybierz główny katalog karty SD.")
            return
        config_path = self.folder / "config.json"
        if config_path.exists():
            answer = QMessageBox.warning(
                self,
                "Nadpisanie konfiguracji",
                "Na karcie istnieje już config.json. Zastąpić go ustawieniami "
                "wprowadzonymi w aplikacji? Istniejące pliki CSV i ich rekordy "
                "pozostaną bez zmian.",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
                QMessageBox.StandardButton.Cancel,
            )
            if answer != QMessageBox.StandardButton.Yes:
                return
        try:
            paths = prepare_sd_card(self.folder, self._configuration())
        except (OSError, ValueError) as error:
            self._show_error(str(error))
            return
        self.csv_paths.clear()
        self._load_table(self.employee_table, EMPLOYEE_FIELDS, EMPLOYEES_FILE)
        self.attendance_table.setRowCount(0)
        self.log_table.setRowCount(0)
        created = [path.name for path in paths[1:]]
        created_text = ", ".join(created) if created else "żadne — pliki już istniały"
        self.status.setText(
            f"Karta przygotowana: {config_path}. Nowe pliki: {created_text}. "
            "Istniejące rekordy CSV zachowano. W trybie offline dodaj pracowników "
            "i zapisz employees.csv przed użyciem terminala. Aby przeglądać "
            "ewidencję lub logi, wskaż ich pliki CSV."
        )

    def _show_error(self, message: str) -> None:
        QMessageBox.critical(self, "RCP2027 — błąd", message)
