"""The Python client against the mock: python3 -m unittest discover -s test/python

Needs the duckdb package of the DuckDB version the extension was built against, the mock running with
DREX_MOCK_URL set, and a release build whose version is that release (make release with OVERRIDE_GIT_DESCRIBE set to
it, as the distribution pipeline does; a local build is a dev version, which the package refuses). Loads the built
httpfs too, so nothing is downloaded. Skips without DREX_MOCK_URL or the built extensions.
"""

import io
import json
import os
import unittest
import urllib.request
from contextlib import redirect_stdout
from pathlib import Path

try:
    import duckdb
except ImportError:
    duckdb = None

MOCK_URL = os.environ.get("DREX_MOCK_URL")
EXTENSIONS = Path(os.environ.get("DREX_EXTENSION_DIR", Path(__file__).resolve().parents[2] / "build/release/extension"))
KEY = "nace_sk_mockKeyForDrexContractTests_000000000000000"


def sent(ns):
    """The /v1/systemone requests the mock received under ns"""
    with urllib.request.urlopen(f"{MOCK_URL}/__log/{ns}") as response:
        return sum(1 for line in response if json.loads(line)["endpoint"] == "/v1/systemone")


@unittest.skipUnless(
    duckdb and MOCK_URL and (EXTENSIONS / "drex/drex.duckdb_extension").exists(),
    "needs the duckdb package, DREX_MOCK_URL and a release build",
)
class PythonClient(unittest.TestCase):
    def connect(self, ns):
        urllib.request.urlopen(f"{MOCK_URL}/__reset/{ns}").read()
        con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
        con.load_extension(str(EXTENSIONS / "httpfs/httpfs.duckdb_extension"))
        con.load_extension(str(EXTENSIONS / "drex/drex.duckdb_extension"))
        con.sql("SET drex_max_rpm = 0")
        con.sql(f"CREATE SECRET drex (TYPE nace, API_KEY '{KEY}', BASE_URL '{MOCK_URL}/ns/{ns}/ok')")
        con.sql("CREATE TABLE t AS SELECT range AS id, 'row ' || range AS txt FROM range(100)")
        return con

    def test_answers_and_types(self):
        con = self.connect("python_answers")
        self.assertEqual(con.sql("SELECT prompt_drex('text', 'Refund?')").fetchall(), [(0.75,)])
        choice = con.sql("SELECT prompt_drex('text', 'Which?', choice := ['red', 'blue']) AS c").fetchall()[0][0]
        self.assertEqual(choice["choice"], "red")
        self.assertEqual([p["value"] for p in choice["probabilities"]], ["blue", "red"])
        self.assertEqual(
            con.execute("SELECT prompt_drex(txt, ?) FROM t WHERE id < ?", ["Refund?", 5]).fetchall(), [(0.75,)] * 5
        )
        self.assertEqual(sent("python_answers"), 7)

    def test_each_read_of_a_relation_sends_again(self):
        con = self.connect("python_relation")
        relation = con.sql("SELECT id, prompt_drex(txt, 'Refund?') AS p FROM t WHERE id < 10")
        self.assertEqual(sent("python_relation"), 0)
        relation.fetchall()
        self.assertEqual(sent("python_relation"), 10)
        relation.fetchall()
        self.assertEqual(sent("python_relation"), 20)
        # A REPL or a notebook shows a relation by running it
        with redirect_stdout(io.StringIO()):
            relation.show()
        repr(relation)
        self.assertEqual(sent("python_relation"), 40)
        self.assertEqual(relation.columns, ["id", "p"])
        self.assertEqual(sent("python_relation"), 40)
        # A table keeps the answers: reading it sends nothing
        relation.to_table("answers")
        con.sql("FROM answers").fetchall()
        con.sql("FROM answers").fetchall()
        self.assertEqual(sent("python_relation"), 50)


if __name__ == "__main__":
    unittest.main()
