import argparse


def generate_tuple(num_of_elements):
    members = ", ".join([f"p{i}" for i in range(1, num_of_elements + 1)])

    case = f"""auto& [{members}] = t;
return std::tie({members});
"""
    if (num_of_elements == 0):
        case = "return std::tie();"
    return f"""if constexpr (arity == {num_of_elements}){{
{case}
}}
"""


def main():
    parser = argparse.ArgumentParser(prog="to_tie generator")
    parser.add_argument("num_of_cases",
                        type=int,
                        help="Number of cases to generate")
    args = parser.parse_args()
    num_of_cases = args.num_of_cases + 1
    with open("to_tie.hpp", "w") as file:
        file.write("""
#pragma once
#include "aggregate_arity.hpp"
namespace detail {
template <typename T, std::size_t arity = aggregate_arity<std::remove_cv_t<T>>::size() - 1>
constexpr auto to_tie( T &t){
                   """)
        cases = " else ".join(
            [generate_tuple(i) for i in range(0, num_of_cases)])
        file.write(cases)
        file.write(
            f'else if constexpr (arity >= {num_of_cases}) {{ static_assert(false,"Generate new to_tie function, the script is located in utils/configuration_store");\n }}'
        )
        file.write("};}")


if __name__ == "__main__":
    main()
