
from __future__ import annotations

import os
import subprocess
import threading
from dataclasses import dataclass
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, GLib, Gtk, Pango


@dataclass
class Result:
    name: str
    health: float
    compression: float
    extraction: float
    compression_gain: float
    extraction_gain: float
    total: int
    verified: int
    original_bytes: int
    compressed_bytes: int
    ratio: float

    @classmethod
    def parse(cls, line: str) -> "Result":
        value = line.removeprefix("RESULT=").split("|")
        if len(value) != 11:
            raise ValueError(f"Resultado inválido: {line}")
        return cls(
            value[0], float(value[1]), float(value[2]), float(value[3]),
            float(value[4]), float(value[5]), int(value[6]), int(value[7]),
            int(value[8]), int(value[9]), float(value[10])
        )


class MainWindow(Gtk.ApplicationWindow):
    HEADERS = (
        "Versión", "Salud", "Compresión", "Descompresión",
        "Aceleración comp.", "Aceleración desc.", "Original",
        "Comprimido", "Razón",
    )
    NAMES = ("Serial", "Paralela", "Concurrente")

    def __init__(self, application: Gtk.Application) -> None:
        super().__init__(application=application)
        self.set_title("Compresor Huffman")
        self.set_default_size(1250, 620)

        self.input_dir: Path | None = None
        self.archive: Path | None = None
        self.output_dir: Path | None = None
        self.rows: dict[str, list[Gtk.Label]] = {}
        self.path_labels: dict[str, Gtk.Label] = {}
        self.action_buttons: list[Gtk.Button] = []

        main = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL, spacing=16,
            margin_top=18, margin_bottom=18, margin_start=18, margin_end=18,
        )
        self.set_child(main)

        title = Gtk.Label(label="Comparación de compresores Huffman")
        title.add_css_class("title-1")
        title.set_xalign(0)
        main.append(title)

        description = Gtk.Label(
            label=("Los cálculos se realizan en comparador.c. "
                   "Python únicamente controla y muestra la interfaz.")
        )
        description.set_xalign(0)
        main.append(description)

        main.append(self._path_row("Entrada", "Seleccionar directorio", self._input))
        main.append(self._path_row("Archivo HUF", "Seleccionar archivo", self._archive))
        main.append(self._path_row("Resultados", "Seleccionar carpeta", self._output))

        actions = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        for text, mode in (
            ("Comprimir con las 3", "compress"),
            ("Descomprimir con las 3", "extract"),
            ("Comparación completa", "compare"),
        ):
            button = Gtk.Button(label=text)
            button.connect("clicked", self._start, mode)
            if mode == "compare":
                button.add_css_class("suggested-action")
            self.action_buttons.append(button)
            actions.append(button)

        clear_button = Gtk.Button(label="Limpiar")
        clear_button.connect("clicked", self._reset)
        self.action_buttons.append(clear_button)
        actions.append(clear_button)

        self.spinner = Gtk.Spinner()
        actions.append(self.spinner)
        self.status = Gtk.Label(label="Listo")
        self.status.set_hexpand(True)
        self.status.set_xalign(0)
        actions.append(self.status)
        main.append(actions)

        scroll = Gtk.ScrolledWindow()
        scroll.set_hexpand(True)
        scroll.set_vexpand(True)
        scroll.set_child(self._table())
        main.append(scroll)

        note = Gtk.Label(
            label=("Comprimir/Comparar: seleccione Entrada y Resultados. "
                   "Descomprimir: seleccione Archivo HUF y Resultados.")
        )
        note.set_xalign(0)
        note.add_css_class("dim-label")
        main.append(note)

    def _path_row(self, title: str, button_text: str, callback) -> Gtk.Box:
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        heading = Gtk.Label(label=f"{title}:")
        heading.set_size_request(110, -1)
        heading.set_xalign(0)
        row.append(heading)
        label = Gtk.Label(label="No seleccionado")
        label.set_hexpand(True)
        label.set_xalign(0)
        label.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        row.append(label)
        self.path_labels[title] = label
        button = Gtk.Button(label=button_text)
        button.connect("clicked", callback, label)
        row.append(button)
        return row

    def _table(self) -> Gtk.Grid:
        grid = Gtk.Grid(column_spacing=14, row_spacing=12)
        for column, text in enumerate(self.HEADERS):
            label = Gtk.Label(label=text)
            label.add_css_class("heading")
            grid.attach(label, column, 0, 1, 1)
        for row_number, name in enumerate(self.NAMES, 1):
            labels = []
            for column in range(len(self.HEADERS)):
                label = Gtk.Label(label=name if column == 0 else "—")
                grid.attach(label, column, row_number, 1, 1)
                labels.append(label)
            self.rows[name] = labels
        return grid

    def _input(self, _button: Gtk.Button, label: Gtk.Label) -> None:
        self._folder_dialog("Seleccionar directorio de entrada", label, "input")

    def _output(self, _button: Gtk.Button, label: Gtk.Label) -> None:
        self._folder_dialog("Seleccionar carpeta de resultados", label, "output")

    def _archive(self, _button: Gtk.Button, label: Gtk.Label) -> None:
        dialog = Gtk.FileChooserNative(
            title="Seleccionar archivo HUF", transient_for=self,
            action=Gtk.FileChooserAction.OPEN,
            accept_label="Seleccionar", cancel_label="Cancelar",
        )
        file_filter = Gtk.FileFilter()
        file_filter.set_name("Archivos Huffman (*.huf)")
        file_filter.add_pattern("*.huf")
        dialog.add_filter(file_filter)

        def response(chooser, result: int) -> None:
            if result == Gtk.ResponseType.ACCEPT:
                selected = chooser.get_file()
                path = selected.get_path() if selected else None
                if path:
                    self.archive = Path(path)
                    label.set_text(path)
            chooser.destroy()

        dialog.connect("response", response)
        dialog.show()

    def _folder_dialog(self, title: str, label: Gtk.Label, target: str) -> None:
        dialog = Gtk.FileChooserNative(
            title=title, transient_for=self,
            action=Gtk.FileChooserAction.SELECT_FOLDER,
            accept_label="Seleccionar", cancel_label="Cancelar",
        )

        def response(chooser, result: int) -> None:
            if result == Gtk.ResponseType.ACCEPT:
                selected = chooser.get_file()
                path_text = selected.get_path() if selected else None
                if path_text:
                    path = Path(path_text)
                    if target == "input":
                        self.input_dir = path
                    else:
                        self.output_dir = path
                    label.set_text(path_text)
            chooser.destroy()

        dialog.connect("response", response)
        dialog.show()

    def _start(self, _button: Gtk.Button, mode: str) -> None:
        if self.output_dir is None:
            self._error("Seleccione la carpeta de resultados.")
            return
        if mode in ("compress", "compare"):
            if self.input_dir is None:
                self._error("Seleccione el directorio de entrada.")
                return
            source = self.input_dir.resolve()
            try:
                self.output_dir.resolve().relative_to(source)
                self._error("Resultados no puede estar dentro de Entrada.")
                return
            except ValueError:
                pass
        else:
            if self.archive is None:
                self._error("Seleccione un archivo .huf.")
                return
            source = self.archive.resolve()

        self._clear()
        self._busy(True, "Ejecutando…")
        threading.Thread(
            target=self._run,
            args=(mode, source, self.output_dir.resolve()),
            daemon=True,
        ).start()

    def _run(self, mode: str, source: Path, output: Path) -> None:
        try:
            comparator = Path(__file__).resolve().parent / "comparador"
            if not comparator.is_file() or not os.access(comparator, os.X_OK):
                raise RuntimeError("No se encontró comparador. Ejecute 'make'.")
            output.mkdir(parents=True, exist_ok=True)
            process = subprocess.run(
                [str(comparator), mode, str(source), str(output)],
                capture_output=True, text=True, check=False,
            )
            if process.returncode != 0:
                raise RuntimeError(process.stderr.strip() or "Falló comparador.")
            lines = process.stdout.splitlines()
            results = [Result.parse(line) for line in lines if line.startswith("RESULT=")]
            run_dir = next(
                (line.removeprefix("RUN_DIRECTORY=") for line in lines
                 if line.startswith("RUN_DIRECTORY=")), ""
            )
            if len(results) != 3:
                raise RuntimeError("No se recibieron los tres resultados.")
            GLib.idle_add(self._display, results, run_dir)
        except Exception as error:
            GLib.idle_add(self._failed, str(error))

    def _display(self, results: list[Result], run_dir: str) -> bool:
        for result in results:
            labels = self.rows[result.name]
            values = (
                result.name,
                "—" if result.health < 0 else
                    f"{result.health:.2f}% ({result.verified}/{result.total})",
                self._seconds(result.compression),
                self._seconds(result.extraction),
                self._percent(result.compression_gain),
                self._percent(result.extraction_gain),
                f"{result.original_bytes / 1_048_576:.2f} MiB",
                f"{result.compressed_bytes / 1_048_576:.2f} MiB",
                f"{result.ratio:.2f}%",
            )
            for label, value in zip(labels, values):
                label.set_text(value)
        self._busy(False, f"Finalizado: {run_dir}" if run_dir else "Finalizado")
        return False

    @staticmethod
    def _seconds(value: float) -> str:
        return "—" if value < 0 else f"{value:.3f} s"

    @staticmethod
    def _percent(value: float) -> str:
        # comparador.c usa -1 para indicar que la métrica no aplica.
        # fue más lenta que la serial.
        return "—" if value == -1.0 else f"{value:.2f}%"

    def _clear(self) -> None:
        for name, labels in self.rows.items():
            labels[0].set_text(name)
            for label in labels[1:]:
                label.set_text("—")

    def _reset(self, _button: Gtk.Button) -> None:
        """Restablece la interfaz sin borrar archivos ni directorios."""
        self.input_dir = None
        self.archive = None
        self.output_dir = None
        for label in self.path_labels.values():
            label.set_text("No seleccionado")
        self._clear()
        self.status.set_text("Listo")

    def _failed(self, message: str) -> bool:
        self._busy(False, "La operación falló")
        self._error(message)
        return False

    def _busy(self, active: bool, text: str) -> None:
        for button in self.action_buttons:
            button.set_sensitive(not active)
        self.status.set_text(text)
        self.spinner.start() if active else self.spinner.stop()

    def _error(self, message: str) -> None:
        dialog = Gtk.AlertDialog()
        dialog.set_message("Error")
        dialog.set_detail(message)
        dialog.set_buttons(["Cerrar"])
        dialog.show(self)


class Application(Gtk.Application):
    def __init__(self) -> None:
        super().__init__(
            application_id="org.proyectocompresor.huffman",
            flags=Gio.ApplicationFlags.NON_UNIQUE,
        )

    def do_activate(self) -> None:
        window = self.props.active_window or MainWindow(self)
        window.present()


if __name__ == "__main__":
    raise SystemExit(Application().run(None))