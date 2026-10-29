from tools.summarize_soa_speed import duration, timing, total_score


def test_duration_and_score_formula():
    assert duration("1:02:03.5") == 3723.5
    assert abs(total_score(100.0, 120.0) - 99.0) < 1e-12
    assert abs(total_score(99.9, 110.4) - 99.0) < 1e-12


def test_timing_parses_internal_and_gnu_time(tmp_path):
    path = tmp_path / "timing.txt"
    path.write_text(
        "processed=608000 bad_rows=0 threads=4 total_elapsed=3.5 load=1.2 "
        "read=0.1 parse=0.2 predict=1.5 format=0.3 write=0.2\n"
        "Elapsed (wall clock) time (h:mm:ss or m:ss): 0:03.75\n",
        encoding="utf-8",
    )
    parsed = timing(path)
    assert parsed["wall"] == 3.75
    assert parsed["predict"] == 1.5
    assert parsed["processed"] == 608000
