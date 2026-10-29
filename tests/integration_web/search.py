from urllib.parse import urlencode

from integration_support import Server, assert_true, event, fnv1a64, write_session
from web_support import web_host


def test_web_search_finds_rows_across_conversations(root, home, *, binary):
    project = root / "search"
    project.mkdir()
    tool = "tool_result"

    def said(*texts):
        return [
            {"role": "user" if index % 2 == 0 else "assistant", "content": text}
            for index, text in enumerate(texts)
        ]

    sessions = {
        "alpha": write_session(
            home, "alpha", said("find the quokka", "a Quokka, a quokka"), cwd=project
        ),
        "beta": write_session(
            home,
            "beta",
            # A result is found by what a person reads, never by the hint its
            # message carries for the model.
            [{"role": "tool", "tool_call_id": "c1", "content": "QUOKKA spotted\n[quokka hint]"}],
            cwd=root,
            kinds=[tool],
            display={
                "ids": [1],
                "next": 2,
                "facts": {"t-c1": {"name": "run", "call_id": "c1", "output": "QUOKKA spotted"}},
            },
        ),
        # An agent is a conversation like any other.
        "agent": write_session(
            home, "agent", said("quokka work"), cwd=project, agent={"parent": "0" * 16}
        ),
        "many": write_session(home, "many", said(*[f"quokka {n}" for n in range(8)]), cwd=project),
        "none": write_session(home, "none", said("nothing here"), cwd=project),
        # Saved escaped, so read without the raw-bytes shortcut.
        "quote": write_session(home, "quote", said('he said "Quokka!" in a café'), cwd=project),
    }
    ids = {name: fnv1a64(str(path)) for name, path in sessions.items()}
    with Server([lambda _, _body: event({"content": "unused"})]) as provider:
        with web_host(binary, root, home, provider.url) as (client, code, _, _):
            client.pair(code)

            def search(**query):
                return client.json(f"/api/search?{urlencode(query)}")[:2]

            status, value = search(q="quokka")
            assert_true(status == 200 and not value["truncated"], value)
            found = {}
            for hit in value["hits"]:
                found.setdefault(hit["session_id"], []).append(hit)
            assert_true(
                set(found) == {ids[name] for name in ("alpha", "beta", "agent", "many", "quote")},
                found,
            )
            # A conversation lists its newest few rows.
            assert_true(
                [hit["snippet"] for hit in found[ids["many"]]]
                == [f"quokka {n}" for n in range(3, 8)],
                found[ids["many"]],
            )
            # Each hit knows how many rows of its conversation hold the words.
            assert_true(all(hit["rows"] == 8 for hit in found[ids["many"]]), found[ids["many"]])
            assert_true(
                [(hit["kind"], hit["count"]) for hit in found[ids["alpha"]]]
                == [("turn", 1), ("message", 2)],
                found[ids["alpha"]],
            )
            assert_true(
                found[ids["beta"]][0]["kind"] == "call"
                and found[ids["alpha"]][0]["title"] == "alpha",
                found,
            )
            for query in ('"quokka!"', "CAFé"):
                status, value = search(q=query)
                assert_true(
                    status == 200
                    and [hit["session_id"] for hit in value["hits"]] == [ids["quote"]],
                    (query, value),
                )
            # Within one conversation, every row, each with its sequence.
            status, value = search(q="quokka", session=ids["many"])
            assert_true(status == 200 and len(value["hits"]) == 8 and not value["truncated"], value)
            sequences = [hit["sequence"] for hit in value["hits"]]
            assert_true(sequences == sorted(sequences), sequences)
            status, value = search(q="quokka", session="f" * 16)
            assert_true(status == 404, value)
            status, value = search(q="q")
            assert_true(status == 400, value)
