import random
import unittest
from parallel_budget_model import Budget, Work, assess_block, MAX_UNITS


def work(units, reads=(), writes=(), known=True):
    return Work(units, frozenset(reads), frozenset(writes), known)


class ParallelBudgetModelTests(unittest.TestCase):
    budget = Budget(total=800, critical_path=200, serial=200, transaction=100)

    def test_independent_work_can_exceed_serial_budget_not_total_budget(self):
        independent = [work(100, writes=[str(i)]) for i in range(8)]
        result = assess_block(independent, self.budget)
        self.assertTrue(result['within_model_budget'])
        self.assertEqual(result['total_work'], 800)
        self.assertEqual(result['critical_path_work'], 100)
        self.assertEqual(assess_block(independent + [work(1)], self.budget)['violations'], ['total'])

    def test_all_hot_work_cannot_hide_behind_total_capacity(self):
        result = assess_block([work(100, writes=['balance'])] * 3, self.budget)
        self.assertEqual(result['total_work'], 300)
        self.assertEqual(result['critical_path_work'], 300)
        self.assertEqual(result['violations'], ['critical_path'])

    def test_shared_reads_parallel_but_raw_war_waw_ordered(self):
        for before, after in ((work(70, writes=['x']), work(30, reads=['x'])),
                              (work(70, reads=['x']), work(30, writes=['x'])),
                              (work(70, writes=['x']), work(30, writes=['x']))):
            self.assertEqual(assess_block([before, after], self.budget)['critical_path_work'], 100)
        readers = [work(70, reads=['x']), work(30, reads=['x'])]
        self.assertEqual(assess_block(readers, self.budget)['critical_path_work'], 70)
        self.assertEqual(assess_block(readers + [work(10, writes=['x'])], self.budget)['critical_path_work'], 80)

    def test_bridge_transaction_joins_different_keys_into_one_long_path(self):
        result = assess_block([work(80, writes=['x']), work(80, reads=['x'], writes=['y']),
                               work(80, reads=['y'], writes=['z'])], self.budget)
        self.assertEqual(result['critical_path_work'], 240)
        self.assertEqual(result['violations'], ['critical_path'])

    def test_unknown_barrier_waits_for_all_prior_and_all_future_work(self):
        result = assess_block([work(50, writes=['x']), work(60, writes=['y']),
                               work(70, known=False), work(80, writes=['never-seen'])], self.budget)
        self.assertEqual(result['completion_work'], [50, 60, 130, 210])
        self.assertEqual(result['serial_barrier_work'], 70)
        self.assertEqual(result['violations'], ['critical_path'])
        all_unknown = assess_block([work(100, known=False)] * 3, self.budget)
        self.assertEqual(all_unknown['violations'], ['critical_path', 'serial'])

    def test_limits_exact_and_no_hardware_worker_count_is_an_input(self):
        inputs = [work(100, writes=['x']), work(100, writes=['x'])]
        first = assess_block(inputs, self.budget)
        for _ in range(16):
            self.assertEqual(assess_block(inputs, self.budget), first)
        self.assertTrue(first['within_model_budget'])
        result = assess_block([work(101)], self.budget)
        self.assertEqual(result['violations'], ['transaction'])

    def test_invalid_inputs_bounds_and_overflow_fail_closed(self):
        for value in (True, 0, -1, 1.5, MAX_UNITS + 1):
            with self.subTest(value=value), self.assertRaises(ValueError):
                work(value)
        with self.assertRaises(ValueError):
            Work(1, {'x'})
        with self.assertRaises(ValueError):
            work(1, reads=['x'], known=False)
        with self.assertRaises(ValueError):
            work(1, reads=[str(i) for i in range(4097)])
        with self.assertRaises(ValueError):
            assess_block([work(1)] * 4097, self.budget)
        unlimited = Budget(MAX_UNITS, MAX_UNITS, MAX_UNITS, MAX_UNITS)
        with self.assertRaises(ValueError):
            assess_block([work(MAX_UNITS), work(1)], unlimited)
        with self.assertRaises(ValueError):
            Budget(100, 100, 100, 101)

    def test_seeded_random_footprints_match_independent_quadratic_dag_oracle(self):
        generator = random.Random(21021)
        budget = Budget(100000, 100000, 100000, 1000)
        for iteration in range(1000):
            transactions = []
            for _ in range(generator.randint(1, 24)):
                known = generator.random() >= .1
                reads = {str(generator.randrange(8)) for _ in range(generator.randrange(4))} if known else set()
                writes = {str(generator.randrange(8)) for _ in range(generator.randrange(4))} if known else set()
                transactions.append(work(generator.randint(1, 100), reads, writes, known))
            ends = []
            for index, current in enumerate(transactions):
                predecessors = [ends[prior] for prior, previous in enumerate(transactions[:index])
                    if not current.known or not previous.known or
                    previous.writes & (current.reads | current.writes) or current.writes & previous.reads]
                ends.append(current.units + max(predecessors, default=0))
            with self.subTest(iteration=iteration):
                self.assertEqual(assess_block(transactions, budget)['completion_work'], ends)


if __name__ == '__main__':
    unittest.main()
